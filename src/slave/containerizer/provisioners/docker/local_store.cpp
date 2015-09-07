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

#include "slave/containerizer/provisioners/docker/local_store.hpp"

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

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/provisioners/docker/store.hpp"
#include "slave/containerizer/provisioners/docker/paths.hpp"

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
  ~LocalStoreProcess() {}

  static Try<process::Owned<LocalStoreProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<DockerImage> get(const std::string& name);

private:
  LocalStoreProcess(
      const Flags& flags,
      Owned<ReferenceStore> _refStore);

  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& staging);

  process::Future<DockerImage> putImage(
      const std::string& name,
      const std::string& staging)

  Result<std::string> getParentId(
      const std::string& staging,
      const std::string& layerId);

  process::Future<Nothing> putLayers(
      const std::string& staging,
      const std::list<std::string>& layers)

  process::Future<Nothing> putLayer(
      const std::string& staging,
      const std::string& id)

  process::Future<Nothing> moveLayer(
      const std::string& staging,
      const std::string& id)

  const Flags flags;

  process::Owned<ReferenceStore> refStore;
};


Try<Owned<Store>> Store::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  hashmap<string, Try<Owned<Store>>(*)(const Flags&, Fetcher*)> creators{
    {"local", &LocalStore::create}
  };

  if (!creators.contains(flags.docker_store)) {
    return Error("Unknown Docker store: " + flags.docker_store);
  }

  return creators[flags.docker_store](flags, fetcher);
}


Try<Owned<Store>> LocalStore::create(
    const Flags& flags,
    Fetcher* fetcher)
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

  Try<Owned<LocalStoreProcess>> process =
    LocalStoreProcess::create(flags, fetcher);
  if (process.isError()) {
    return Error(process.error());
  }

  Try<Owned<ReferenceStore>> refStore = ReferenceStore::create(flags);
  if (refStore.isError()) {
    return Error(refStore);
  }

  return Owned<Store>(new LocalStore(process.get(), refStore.get()));
}


LocalStore::LocalStore(
    Owned<LocalStoreProcess> process,
    Owned<ReferenceStore> refStore)
  : process(process),
    _refStore(refStore)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


LocalStore::~LocalStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Option<DockerImage>> LocalStore::get(const string& name)
{
  return dispatch(process.get(), &LocalStoreProcess::get, name);
}


Try<Owned<LocalStoreProcess>> LocalStoreProcess::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  return Owned<LocalStoreProcess>(new LocalStoreProcess(flags));
}


LocalStoreProcess::LocalStoreProcess(const Flags& flags)
  : flags(flags), refStore(ReferenceStore::create(flags).get()) {}


Future<DockerImage> LocalStoreProcess::get(const string& name)
{
  Option<DockerImage> image = refStore->get(name);
  if (image.isSome()) {
    return image.get();
  }

  string tarPath =
    paths::getLocalImageTarPath(flags.docker_discovery_local_dir, name);
  if (!os::exists(tarPath)) {
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


Future<Nothing> LocalStoreProcess::untarImage(
    const string& tarPath,
    const string& staging)
{
  VLOG(1) << "Untarring image at: " << tarPath;

  // Untar discovery_local_dir/name.tar into staging/.
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
    const std::string& name,
    const string& staging)
{
  ImageName imageName(name);

  Try<string> value = os::read(paths::getLocalImageRepositoriesPath(staging));
  if (value.isError()) {
    return Failure("Failed to read repository JSON: " + value.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(value.get());
  if (json.isError()) {
    return Failure("Failed to parse JSON: " + json.error());
  }

  Result<JSON::Object> repositoryValue =
    json.get().find<JSON::Object>(imageName.repo);
  if (repositoryValue.isError()) {
    return Failure("Failed to find repository: " + repositoryValue.error());
  } else if (repositoryValue.isNone()) {
    return Failure("Repository '" + imageName.repo + "' is not found");
  }

  JSON::Object repositoryJson = repositoryValue.get();

  // We don't use JSON find here because a tag might contain a '.'.
  std::map<string, JSON::Value>::const_iterator entry =
    repositoryJson.values.find(imageName.tag);
  if (entry == repositoryJson.values.end()) {
    return Failure("Tag '" + imageName.tag + "' is not found");
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

  list<string> layers;
  layers.push_back(layerId);
  Result<string> parentId = getParentId(staging, layerId);
  while(parentId.isSome()) {
    layers.push_front(parentId.get());
    parentId = getParentId(staging, parentId.get());
  }
  if (parentId.isError()) {
    return Failure("Failed to obtain parent layer id: " + parentId.error());
  }

  return putLayers(staging, layers)
    .then([=]() -> Future<DockerImage> {
      return refStore->put(name, layers);
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
    const list<string>& layers)
{
  list<Future<Nothing>> futures{ Nothing() };
  foreach (const string& layer, layers) {
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
  if (!os::exists()) {
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
