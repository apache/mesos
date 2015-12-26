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

#include <list>
#include <map>
#include <vector>

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/local_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

using namespace process;

using std::list;
using std::map;
using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class LocalPullerProcess : public Process<LocalPullerProcess>
{
public:
  LocalPullerProcess(const string& _archivesDir) : archivesDir(_archivesDir) {}

  ~LocalPullerProcess() {}

  Future<list<pair<string, string>>> pull(
      const Image::Name& name,
      const string& directory);

private:
  Future<list<pair<string, string>>> putImage(
      const Image::Name& name,
      const string& directory);

  Future<list<pair<string, string>>> putLayers(
      const string& directory,
      const vector<string>& layerIds);

  const string archivesDir;
};


Try<Owned<Puller>> LocalPuller::create(const Flags& flags)
{
  // This should already been verified at puller.cpp.
  if (!strings::startsWith(flags.docker_registry, "file://")) {
    return Error("Expecting registry url to have file:// scheme");
  }

  const string archivesDir = strings::remove(
      flags.docker_registry,
      "file://",
      strings::Mode::PREFIX);

  Owned<LocalPullerProcess> process(new LocalPullerProcess(archivesDir));

  return Owned<Puller>(new LocalPuller(process));
}


LocalPuller::LocalPuller(Owned<LocalPullerProcess>& _process)
  : process(_process)
{
  spawn(process.get());
}


LocalPuller::~LocalPuller()
{
  terminate(process.get());
  wait(process.get());
}


Future<list<pair<string, string>>> LocalPuller::pull(
    const Image::Name& name,
    const Path& directory)
{
  return dispatch(process.get(), &LocalPullerProcess::pull, name, directory);
}


Future<list<pair<string, string>>> LocalPullerProcess::pull(
    const Image::Name& name,
    const string& directory)
{
  const string tarPath = paths::getImageArchiveTarPath(
      archivesDir,
      stringify(name));

  if (!os::exists(tarPath)) {
    return Failure("Failed to find archive for image '" + stringify(name) +
                   "' at '" + tarPath + "'");
  }

  VLOG(1) << "Untarring image from '" << tarPath
          << "' to '" << directory << "'";

  return untar(tarPath, directory)
    .then(defer(self(), &Self::putImage, name, directory));
}


static Result<string> getParentId(
    const string& directory,
    const string& layerId)
{
  Try<string> manifest =
    os::read(paths::getImageArchiveLayerManifestPath(directory, layerId));

  if (manifest.isError()) {
    return Error("Failed to read manifest: " + manifest.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(manifest.get());
  if (json.isError()) {
    return Error("Failed to parse manifest: " + json.error());
  }

  Result<JSON::String> parentId = json.get().find<JSON::String>("parent");
  if (parentId.isNone() || (parentId.isSome() && parentId.get() == "")) {
    return None();
  } else if (parentId.isError()) {
    return Error("Failed to read parent of layer: " + parentId.error());
  }

  return parentId.get().value;
}


Future<list<pair<string, string>>> LocalPullerProcess::putImage(
    const Image::Name& name,
    const string& directory)
{
  Try<string> value =
    os::read(paths::getImageArchiveRepositoriesPath(directory));

  if (value.isError()) {
    return Failure("Failed to read repository JSON: " + value.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(value.get());
  if (json.isError()) {
    return Failure("Failed to parse JSON: " + json.error());
  }

  Result<JSON::Object> repositoryValue =
    json.get().find<JSON::Object>(name.repository());

  if (repositoryValue.isError()) {
    return Failure("Failed to find repository: " + repositoryValue.error());
  } else if (repositoryValue.isNone()) {
    return Failure("Repository '" + name.repository() + "' is not found");
  }

  const JSON::Object repositoryJson = repositoryValue.get();

  // We don't use JSON find here because a tag might contain a '.'.
  map<string, JSON::Value>::const_iterator entry =
    repositoryJson.values.find(name.tag());

  if (entry == repositoryJson.values.end()) {
    return Failure("Tag '" + name.tag() + "' is not found");
  } else if (!entry->second.is<JSON::String>()) {
    return Failure("Tag JSON value expected to be JSON::String");
  }

  const string layerId = entry->second.as<JSON::String>().value;

  vector<string> layerIds;
  layerIds.push_back(layerId);
  Result<string> parentId = getParentId(directory, layerId);

  while (parentId.isSome()) {
    layerIds.insert(layerIds.begin(), parentId.get());
    parentId = getParentId(directory, parentId.get());
  }

  if (parentId.isError()) {
    return Failure("Failed to find parent layer id of layer '" + layerId +
                   "': " + parentId.error());
  }

  return putLayers(directory, layerIds);
}


Future<list<pair<string, string>>> LocalPullerProcess::putLayers(
    const string& directory,
    const vector<string>& layerIds)
{
  list<Future<pair<string, string>>> futures;
  foreach (const string& layerId, layerIds) {
    const string tarredLayer =
      paths::getImageArchiveLayerTarPath(directory, layerId);
    futures.push_back(untarLayer(tarredLayer, directory, layerId));
  }

  return collect(futures);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
