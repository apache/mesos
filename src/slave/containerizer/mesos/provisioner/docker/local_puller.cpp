// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <vector>

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include "common/command_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/local_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"

using namespace process;

namespace spec = docker::spec;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class LocalPullerProcess : public Process<LocalPullerProcess>
{
public:
  LocalPullerProcess(const string& _storeDir, const string& _archivesDir)
    : ProcessBase(process::ID::generate("docker-provisioner-local-puller")),
      storeDir(_storeDir),
      archivesDir(_archivesDir) {}

  ~LocalPullerProcess() {}

  Future<vector<string>> pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend);

private:
  Future<vector<string>> _pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend);

  Result<string> getParentLayerId(
      const string& directory,
      const string& layerId);

  Future<Nothing> extractLayers(
      const string& directory,
      const vector<string>& layerIds,
      const string& backend);

  Future<Nothing> extractLayer(
      const string& directory,
      const string& layerId,
      const string& backend);

  const string storeDir;
  const string archivesDir;
};


Try<Owned<Puller>> LocalPuller::create(const Flags& flags)
{
  // This should already been verified at puller.cpp.
  if (!strings::startsWith(flags.docker_registry, "/")) {
    return Error("Expecting registry url starting with '/'");
  }

  VLOG(1) << "Creating local puller with docker registry '"
          << flags.docker_registry << "'";

  Owned<LocalPullerProcess> process(
      new LocalPullerProcess(flags.docker_store_dir, flags.docker_registry));

  return Owned<Puller>(new LocalPuller(process));
}


LocalPuller::LocalPuller(Owned<LocalPullerProcess> _process)
  : process(_process)
{
  spawn(process.get());
}


LocalPuller::~LocalPuller()
{
  terminate(process.get());
  wait(process.get());
}


Future<vector<string>> LocalPuller::pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend)
{
  return dispatch(
      process.get(),
      &LocalPullerProcess::pull,
      reference,
      directory,
      backend);
}


Future<vector<string>> LocalPullerProcess::pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend)
{
  // TODO(jieyu): We need to handle the case where the image reference
  // contains a slash '/'.
  const string tarPath = paths::getImageArchiveTarPath(
      archivesDir,
      stringify(reference));

  if (!os::exists(tarPath)) {
    return Failure(
        "Failed to find archive for image '" +
        stringify(reference) + "' at '" + tarPath + "'");
  }

  VLOG(1) << "Untarring image '" << reference
          << "' from '" << tarPath
          << "' to '" << directory << "'";

  return command::untar(Path(tarPath), Path(directory))
    .then(defer(self(), &Self::_pull, reference, directory, backend));
}


Future<vector<string>> LocalPullerProcess::_pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend)
{
  // We first parse the 'repositories' JSON file to get the top most
  // layer id for the image.
  Try<string> _repositories = os::read(path::join(directory, "repositories"));
  if (_repositories.isError()) {
    return Failure("Failed to read 'repositories': " + _repositories.error());
  }

  VLOG(1) << "The repositories JSON file for image '" << reference
          << "' is '" << _repositories.get() << "'";

  Try<JSON::Object> repositories =
    JSON::parse<JSON::Object>(_repositories.get());

  if (repositories.isError()) {
    return Failure("Failed to parse 'repositories': " + repositories.error());
  }

  // We are looking for the topmost layer, so we know that is it OK to
  // use at() rather than find() on the JSON object.
  Result<JSON::Object> repository =
    repositories->at<JSON::Object>(reference.repository());

  // If we didn't find the bare repository name, try
  // with the registry-qualified name. This would look like
  // "registry.example.com/image".
  if (repository.isNone() && reference.has_registry()) {
    repository = repositories->at<JSON::Object>(
        path::join(reference.registry(), reference.repository()));
  }

  if (repository.isError()) {
    return Failure(
        "Failed to find repository '" + reference.repository() +
        "' in 'repositories': " + repository.error());
  } else if (repository.isNone()) {
    return Failure(
        "Repository '" + reference.repository() + "' does not "
        "exist in 'repositories'");
  }

  const string tag = reference.has_tag()
    ? reference.tag()
    : "latest";

  // NOTE: We don't use JSON find here since a tag might contain '.'.
  Result<JSON::String> layerId = repository->at<JSON::String>(tag);

  if (layerId.isError()) {
    return Failure(
        "Failed to access layer id '" + tag + "': " + layerId.error());
  } else if (layerId.isNone()) {
    return Failure("Layer id '" + tag + "' is not found");
  }

  // Do a traverse to find all parent image layer ids. Here, we assume
  // that all the parent layers are part of the archive tar, thus are
  // already extracted under 'directory'.
  vector<string> layerIds = { layerId->value };
  Result<string> parentLayerId = getParentLayerId(directory, layerId->value);
  while (parentLayerId.isSome()) {
    // NOTE: We put parent layer ids in front because that's what the
    // provisioner backends assume.
    layerIds.insert(layerIds.begin(), parentLayerId.get());
    parentLayerId = getParentLayerId(directory, parentLayerId.get());
  }

  if (parentLayerId.isError()) {
    return Failure(
        "Failed to find parent layer id for layer '" + layerId->value +
        "': " + parentLayerId.error());
  }

  return extractLayers(directory, layerIds, backend)
    .then([layerIds]() -> vector<string> { return layerIds; });
}


Result<string> LocalPullerProcess::getParentLayerId(
    const string& directory,
    const string& layerId)
{
  const string layerPath = path::join(directory, layerId);

  Try<string> _manifest = os::read(paths::getImageLayerManifestPath(layerPath));
  if (_manifest.isError()) {
    return Error("Failed to read manifest: " + _manifest.error());
  }

  Try<JSON::Object> manifest = JSON::parse<JSON::Object>(_manifest.get());
  if (manifest.isError()) {
    return Error("Failed to parse manifest: " + manifest.error());
  }

  Result<JSON::Value> parentLayerId = manifest->find<JSON::Value>("parent");
  if (parentLayerId.isError()) {
    return Error("Failed to parse 'parent': " + parentLayerId.error());
  } else if (parentLayerId.isNone()) {
    return None();
  } else if (parentLayerId->is<JSON::Null>()) {
    return None();
  } else if (!parentLayerId->is<JSON::String>()) {
    return Error("Unexpected 'parent' type");
  }

  const string id = parentLayerId->as<JSON::String>().value;
  if (id == "") {
    return None();
  } else {
    return id;
  }
}


Future<Nothing> LocalPullerProcess::extractLayers(
    const string& directory,
    const vector<string>& layerIds,
    const string& backend)
{
  list<Future<Nothing>> futures;
  foreach (const string& layerId, layerIds) {
    // Check if the layer is already in the store. If yes, skip the
    // unnecessary extracting.
    if (os::exists(paths::getImageLayerRootfsPath(
            storeDir,
            layerId,
            backend))) {
      continue;
    }

    futures.push_back(extractLayer(directory, layerId, backend));
  }

  return collect(futures)
    .then([]() { return Nothing(); });
}


Future<Nothing> LocalPullerProcess::extractLayer(
    const string& directory,
    const string& layerId,
    const string& backend)
{
  const string layerPath = path::join(directory, layerId);
  const string tar = paths::getImageLayerTarPath(layerPath);
  const string rootfs = paths::getImageLayerRootfsPath(layerPath, backend);

  VLOG(1) << "Extracting layer tar ball '" << tar
          << " to rootfs '" << rootfs << "'";

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" + rootfs + "'"
        ": " + mkdir.error());
  }

  return command::untar(Path(tar), Path(rootfs))
    .then([tar]() -> Future<Nothing> {
      // Remove the tar after the extraction.
      Try<Nothing> rm = os::rm(tar);
      if (rm.isError()) {
        return Failure(
          "Failed to remove '" + tar + "' "
          "after extraction: " + rm.error());
      }

      return Nothing();
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
