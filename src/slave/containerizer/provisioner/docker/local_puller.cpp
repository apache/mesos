/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <list>
#include <vector>

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/provisioner/docker/local_puller.hpp"
#include "slave/containerizer/provisioner/docker/paths.hpp"
#include "slave/containerizer/provisioner/docker/store.hpp"

using namespace process;

using std::list;
using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class LocalPullerProcess : public process::Process<LocalPullerProcess>
{
public:
  LocalPullerProcess(const Flags& _flags) : flags(_flags) {}

  ~LocalPullerProcess() {}

  process::Future<list<pair<string, string>>> pull(
      const Image::Name& name,
      const string& directory);

private:
  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& directory);

  process::Future<list<pair<string, string>>> putImage(
      const Image::Name& name,
      const std::string& directory);

  process::Future<list<pair<string, string>>> putLayers(
      const std::string& directory,
      const std::vector<std::string>& layerIds);

  process::Future<pair<string, string>> putLayer(
      const std::string& directory,
      const std::string& layerId);

  const Flags flags;
};


LocalPuller::LocalPuller(const Flags& flags)
{
  process = Owned<LocalPullerProcess>(new LocalPullerProcess(flags));
  process::spawn(process.get());
}


LocalPuller::~LocalPuller()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<list<pair<string, string>>> LocalPuller::pull(
    const Image::Name& name,
    const string& directory)
{
  return dispatch(process.get(), &LocalPullerProcess::pull, name, directory);
}


Future<list<pair<string, string>>> LocalPullerProcess::pull(
    const Image::Name& name,
    const string& directory)
{
  string tarPath = paths::getImageArchiveTarPath(
      flags.docker_local_archives_dir,
      stringify(name));

  if (!os::exists(tarPath)) {
    return Failure("Failed to find archive for image '" + stringify(name) +
                   "' at '" + tarPath + "'");
  }

  return untarImage(tarPath, directory)
    .then(defer(self(), &Self::putImage, name, directory));
}


Future<Nothing> LocalPullerProcess::untarImage(
    const string& tarPath,
    const string& directory)
{
  VLOG(1) << "Untarring image from '" << directory
          << "' to '" << tarPath << "'";

  // Untar store_discovery_local_dir/name.tar into directory/.
  // TODO(tnachen): Terminate tar process when slave exits.
  vector<string> argv = {
    "tar",
    "-C",
    directory,
    "-x",
    "-f",
    tarPath
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure("Failed to create tar subprocess: " + s.error());
  }

  return s.get().status()
    .then([tarPath](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap status for tar subprocess in " +
                        tarPath);
      }
      if (!WIFEXITED(status.get()) || WEXITSTATUS(status.get()) != 0) {
          return Failure("Untar image failed with exit code: " +
                          WSTRINGIFY(status.get()));
      }

      return Nothing();
    });
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

  JSON::Object repositoryJson = repositoryValue.get();

  // We don't use JSON find here because a tag might contain a '.'.
  std::map<string, JSON::Value>::const_iterator entry =
    repositoryJson.values.find(name.tag());
  if (entry == repositoryJson.values.end()) {
    return Failure("Tag '" + name.tag() + "' is not found");
  } else if (!entry->second.is<JSON::String>()) {
    return Failure("Tag JSON value expected to be JSON::String");
  }

  string layerId = entry->second.as<JSON::String>().value;

  Try<string> manifest =
    os::read(paths::getImageArchiveLayerManifestPath(directory, layerId));
  if (manifest.isError()) {
    return Failure("Failed to read manifest: " + manifest.error());
  }

  Try<JSON::Object> manifestJson = JSON::parse<JSON::Object>(manifest.get());
  if (manifestJson.isError()) {
    return Failure("Failed to parse manifest: " + manifestJson.error());
  }

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
    futures.push_back(putLayer(directory, layerId));
  }

  return collect(futures);
}


Future<pair<string, string>> LocalPullerProcess::putLayer(
    const string& directory,
    const string& layerId)
{
  // We untar the layer from source into a directory, then move the
  // layer into store. We do this instead of untarring directly to
  // store to make sure we don't end up with partially untarred layer
  // rootfs.

  string localRootfsPath =
    paths::getImageArchiveLayerRootfsPath(directory, layerId);

  // Image layer has been untarred but is not present in the store directory.
  if (os::exists(localRootfsPath)) {
    LOG(WARNING) << "Image layer '" << layerId << "' rootfs present at but not "
                 << "in store directory '" << localRootfsPath << "'. Removing "
                 << "staged rootfs and untarring layer again.";

    Try<Nothing> rmdir = os::rmdir(localRootfsPath);
    if (rmdir.isError()) {
      return Failure("Failed to remove incomplete staged rootfs for layer '" +
                     layerId + "': " + rmdir.error());
    }
  }

  Try<Nothing> mkdir = os::mkdir(localRootfsPath);
  if (mkdir.isError()) {
    return Failure("Failed to create rootfs path '" + localRootfsPath +
                   "': " + mkdir.error());
  }

  // Untar directory/id/layer.tar into directory/id/rootfs.
  // The tar file will be removed when the staging directory is
  // removed.
  vector<string> argv = {
    "tar",
    "-C",
    localRootfsPath,
    "-x",
    "-f",
    paths::getImageArchiveLayerTarPath(directory, layerId)
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure("Failed to create tar subprocess: " + s.error());
  }

  return s.get().status()
    .then([directory, layerId](
        const Option<int>& status) -> Future<pair<string, string>> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to untar image");
      } else if (!WIFEXITED(status.get()) || WEXITSTATUS(status.get()) != 0) {
        return Failure("Untar failed with exit code: " +
                        WSTRINGIFY(status.get()));
      }

      const string rootfsPath =
        paths::getImageArchiveLayerRootfsPath(directory, layerId);

      if (!os::exists(rootfsPath)) {
        return Failure("Failed to find the rootfs path after extracting layer"
                       " '" + layerId + "'");
      }

      return pair<string, string>(layerId, rootfsPath);
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
