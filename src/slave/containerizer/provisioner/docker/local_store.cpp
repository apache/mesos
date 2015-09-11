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

#include "common/status_utils.hpp"

#include "slave/containerizer/provisioner/docker/local_store.hpp"
#include "slave/containerizer/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/provisioner/docker/paths.hpp"
#include "slave/containerizer/provisioner/docker/store.hpp"

#include "slave/flags.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class LocalStoreProcess : public process::Process<LocalStoreProcess>
{
public:
  LocalStoreProcess(
      const Flags& _flags,
      Owned<MetadataManager> _metadataManager)
    : flags(_flags), metadataManager(_metadataManager) {}

  ~LocalStoreProcess() {}

  static Try<process::Owned<LocalStoreProcess>> create(const Flags& flags);

  process::Future<vector<string>> get(const Image& image);

  process::Future<Nothing> recover();

private:
  process::Future<DockerImage> _get(
      const DockerImage::Name& name,
      const Option<DockerImage>& image);

  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& staging);

  process::Future<DockerImage> putImage(
      const DockerImage::Name& name,
      const std::string& staging);

  Result<std::string> getParentId(
      const std::string& staging,
      const std::string& layerId);

  process::Future<Nothing> putLayers(
      const std::string& staging,
      const std::list<std::string>& layerIds);

  process::Future<Nothing> putLayer(
      const std::string& staging,
      const std::string& id);

  process::Future<Nothing> moveLayer(
      const std::string& staging,
      const std::string& id);

  const Flags flags;
  process::Owned<MetadataManager> metadataManager;
};


Try<Owned<slave::Store>> LocalStore::create(const Flags& flags)
{
  if (!os::exists(flags.docker_store_dir)) {
    Try<Nothing> mkdir = os::mkdir(flags.docker_store_dir);
    if (mkdir.isError()) {
      return Error("Failed to create Docker store directory: " + mkdir.error());
    }
  }

  if (!os::exists(paths::getStagingDir(flags.docker_store_dir))) {
    Try<Nothing> mkdir =
      os::mkdir(paths::getStagingDir(flags.docker_store_dir));
    if (mkdir.isError()) {
      return Error("Failed to create Docker store staging directory: " +
                   mkdir.error());
    }
  }

  Try<Owned<MetadataManager>> metadataManager = MetadataManager::create(flags);
  if (metadataManager.isError()) {
    return Error(metadataManager.error());
  }

  Owned<LocalStoreProcess> process(
      new LocalStoreProcess(flags, metadataManager.get()));

  return Owned<slave::Store>(new LocalStore(process));
}


LocalStore::LocalStore(Owned<LocalStoreProcess> _process) : process(_process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


LocalStore::~LocalStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<vector<string>> LocalStore::get(const Image& image)
{
  return dispatch(process.get(), &LocalStoreProcess::get, image);
}


Future<Nothing> LocalStore::recover()
{
  return dispatch(process.get(), &LocalStoreProcess::recover);
}


Future<vector<string>> LocalStoreProcess::get(const Image& image)
{
  CHECK_EQ(image.type(), Image::DOCKER);

  Try<DockerImage::Name> dockerName = parseName(image.docker().name());
  if (dockerName.isError()) {
    return Failure("Unable to parse docker image name: " + dockerName.error());
  }

  return metadataManager->get(dockerName.get())
    .then(defer(self(), &Self::_get, dockerName.get(), lambda::_1))
    .then([](const DockerImage& dockerImage) {
      vector<string> layers;
      foreach (const string& layer, dockerImage.layer_ids()) {
        layers.push_back(layer);
      }

      return layers;
    });
}


Future<DockerImage> LocalStoreProcess::_get(
    const DockerImage::Name& name,
    const Option<DockerImage>& image)
{
  if (image.isSome()) {
    return image.get();
  }

  string tarPath = paths::getLocalImageTarPath(
      flags.docker_store_discovery_local_dir,
      stringify(name));

  if (!os::exists(tarPath)) {
    VLOG(1) << "Unable to find image in local store with path: " << tarPath;
    return Failure("No Docker image tar archive found");
  }

  // Create a temporary staging directory.
  Try<string> staging =
    os::mkdtemp(paths::getTempStaging(flags.docker_store_dir));
  if (staging.isError()) {
    return Failure("Failed to create a staging directory");
  }

  return untarImage(tarPath, staging.get())
    .then(defer(self(), &Self::putImage, name, staging.get()));
}


Future<Nothing> LocalStoreProcess::recover()
{
  return metadataManager->recover();
}


Future<Nothing> LocalStoreProcess::untarImage(
    const string& tarPath,
    const string& staging)
{
  VLOG(1) << "Untarring image at: " << tarPath;

  // Untar store_discovery_local_dir/name.tar into staging/.
  vector<string> argv = {
    "tar",
    "-C",
    staging,
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
    .then([=](const Option<int>& status) -> Future<Nothing> {
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


Future<DockerImage> LocalStoreProcess::putImage(
    const DockerImage::Name& name,
    const string& staging)
{
  Try<string> value = os::read(paths::getLocalImageRepositoriesPath(staging));
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
    os::read(paths::getLocalImageLayerManifestPath(staging, layerId));
  if (manifest.isError()) {
    return Failure("Failed to read manifest: " + manifest.error());
  }

  Try<JSON::Object> manifestJson = JSON::parse<JSON::Object>(manifest.get());
  if (manifestJson.isError()) {
    return Failure("Failed to parse manifest: " + manifestJson.error());
  }

  list<string> layerIds;
  layerIds.push_back(layerId);
  Result<string> parentId = getParentId(staging, layerId);
  while(parentId.isSome()) {
    layerIds.push_front(parentId.get());
    parentId = getParentId(staging, parentId.get());
  }
  if (parentId.isError()) {
    return Failure("Failed to obtain parent layer id: " + parentId.error());
  }

  return putLayers(staging, layerIds)
    .then([=]() -> Future<DockerImage> {
      return metadataManager->put(name, layerIds);
    });
}


Result<string> LocalStoreProcess::getParentId(
    const string& staging,
    const string& layerId)
{
  Try<string> manifest =
    os::read(paths::getLocalImageLayerManifestPath(staging, layerId));
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


Future<Nothing> LocalStoreProcess::putLayers(
    const string& staging,
    const list<string>& layerIds)
{
  list<Future<Nothing>> futures{ Nothing() };
  foreach (const string& layer, layerIds) {
    futures.push_back(
        futures.back().then(
          defer(self(), &Self::putLayer, staging, layer)));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Nothing> LocalStoreProcess::putLayer(
    const string& staging,
    const string& id)
{
  // We untar the layer from source into a staging directory, then
  // move the layer into store. We do this instead of untarring
  // directly to store to make sure we don't end up with partially
  // untarred layer rootfs.

  // Check if image layer rootfs is already in store.
  if (os::exists(paths::getImageLayerRootfsPath(flags.docker_store_dir, id))) {
    VLOG(1) << "Image layer '" << id << "' rootfs already in store. "
            << "Skipping put.";
    return Nothing();
  }

  const string imageLayerPath =
    paths::getImageLayerPath(flags.docker_store_dir, id);
  if (!os::exists(imageLayerPath)) {
    Try<Nothing> mkdir = os::mkdir(imageLayerPath);
    if (mkdir.isError()) {
      return Failure("Failed to create Image layer directory '" +
                     imageLayerPath + "': " + mkdir.error());
    }
  }

  // Image layer has been untarred but is not present in the store directory.
  string localRootfsPath = paths::getLocalImageLayerRootfsPath(staging, id);
  if (os::exists(localRootfsPath)) {
    LOG(WARNING) << "Image layer '" << id << "' rootfs present at but not in "
                 << "store directory '" << localRootfsPath << "'. Removing "
                 << "staged rootfs and untarring layer again.";
    Try<Nothing> rmdir = os::rmdir(localRootfsPath);
    if (rmdir.isError()) {
      return Failure("Failed to remove incomplete staged rootfs for layer '" +
                     id + "': " + rmdir.error());
    }
  }

  Try<Nothing> mkdir = os::mkdir(localRootfsPath);
  if (mkdir.isError()) {
    return Failure("Failed to create rootfs path '" + localRootfsPath + "': " +
                   mkdir.error());
  }
  // Untar staging/id/layer.tar into staging/id/rootfs.
  vector<string> argv = {
    "tar",
    "-C",
    localRootfsPath,
    "-x",
    "-f",
    paths::getLocalImageLayerTarPath(staging, id)
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
    .then([=](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to untar image");
      } else if (!WIFEXITED(status.get()) || WEXITSTATUS(status.get()) != 0) {
        return Failure("Untar failed with exit code: " +
                        WSTRINGIFY(status.get()));
      }

      return moveLayer(staging, id);
    });
}


Future<Nothing> LocalStoreProcess::moveLayer(
    const string& staging,
    const string& id)
{
  Try<Nothing> status = os::rename(
      paths::getLocalImageLayerRootfsPath(staging, id),
      paths::getImageLayerRootfsPath(flags.docker_store_dir, id));

  if (status.isError()) {
    return Failure("Failed to move layer to store directory: "
                   + status.error());
  }

  return Nothing();
}


} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
