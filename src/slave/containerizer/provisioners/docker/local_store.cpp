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
#include "slave/flags.hpp"

#include "slave/containerizer/provisioners/docker/store.hpp"

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

  process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  process::Future<Option<DockerImage>> get(const std::string& name);

private:
  LocalStoreProcess(const Flags& flags);

  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& staging);

  process::Future<DockerImage> putImage(
      const std::string& name,
      const std::string& staging,
      const std::string& sandbox);

  Result<std::string> getParentId(
      const std::string& staging,
      const std::string& layerId);

  process::Future<Nothing> putLayers(
      const std::string& staging,
      const std::list<std::string>& layers,
      const std::string& sandbox);

  process::Future<Nothing> untarLayer(
      const std::string& staging,
      const std::string& id,
      const std::string& sandbox);

  process::Future<Nothing> moveLayer(
      const std::string& staging,
      const std::string& id,
      const std::string& sandbox);

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
  Try<Owned<LocalStoreProcess>> process =
    LocalStoreProcess::create(flags, fetcher);
  if (process.isError()) {
    return Error("Failed to create store: " + process.error());
  }

  return Owned<Store>(new LocalStore(process.get()));
}


LocalStore::LocalStore(Owned<LocalStoreProcess> process)
  : process(process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


LocalStore::~LocalStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<DockerImage> LocalStore::put(
    const string& name,
    const string& sandbox)
{
  return dispatch(process.get(), &LocalStoreProcess::put, name, sandbox);
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


Future<DockerImage> LocalStoreProcess::put(
    const string& name,
    const string& sandbox)
{
  string tarName = name + ".tar";
  Try<string> tarPath = path::join(flags.docker_discovery_local_dir, tarName);
  if (tarPath.isError()) {
    return Failure(tarPath.error());
  }
  if (!os::exists(tarPath.get())) {
    return Failure("No Docker image tar archive found: " + name);
  }

  // Create a temporary staging directory.
  Try<string> staging = os::mkdtemp();
  if (staging.isError()) {
    return Failure("Failed to create a staging directory");
  }

  return untarImage(tarPath.get(), staging.get())
    .then(defer(self(), &Self::putImage, name, staging.get(), sandbox));
}


Future<Nothing> LocalStoreProcess::untarImage(
    const string& tarPath,
    const string& staging)
{
  LOG(INFO) << "Untarring image at: " << tarPath;

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
    const string& staging,
    const string& sandbox)
{
  ImageName imageName(name);
  // Read repository json.
  Try<string> repoPath = path::join(staging, "repositories");
  if (repoPath.isError()) {
    return Failure("Failed to create path to repository: " + repoPath.error());
  }

  Try<string> value = os::read(repoPath.get());
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

  Try<string> layerPath = path::join(
      staging,
      entry->second.as<JSON::String>().value);
  if (layerPath.isError()) {
    return Failure("Failed to create path to image layer: " +
                    layerPath.error());
  }
  string layerId = entry->second.as<JSON::String>().value;

  Try<string> manifest = os::read(path::join(staging, layerId, "json"));
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

  return putLayers(staging, layers, sandbox)
    .then([=]() -> Future<DockerImage> {
      return refStore->put(name, layers);
    });
}


Result<string> LocalStoreProcess::getParentId(
    const string& staging,
    const string& layerId)
{
  Try<string> manifest = os::read(path::join(staging, layerId, "json"));
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
    const list<string>& layers,
    const string& sandbox)
{
  list<Future<Nothing>> futures{ Nothing() };
  foreach (const string& layer, layers) {
    futures.push_back(
        futures.back().then(
          defer(self(), &Self::untarLayer, staging, layer, sandbox)));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Nothing> LocalStoreProcess::untarLayer(
    const string& staging,
    const string& id,
    const string& sandbox)
{
  // Check if image layer is already in store.
  if (os::exists(path::join(flags.docker_store_dir, id))) {
    VLOG(1) << "Image layer: " << id << " already in store. Skipping untar"
            << " and putLayer.";
    return Nothing();
  }

  // Image layer has been untarred but is not present in the store directory.
  if (os::exists(path::join(staging, id, "rootfs"))) {
    LOG(WARNING) << "Image layer rootfs present at but not in store directory: "
                << path::join(staging, id) << "Skipping untarLayer.";
    return moveLayer(staging, id, sandbox);
  }

  os::mkdir(path::join(staging, id, "rootfs"));
  // Untar staging/id/layer.tar into staging/id/rootfs.
  vector<string> argv = {
    "tar",
    "-C",
    path::join(staging, id, "rootfs"),
    "-x",
    "-f",
    path::join(staging, id, "layer.tar")
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
      Try<string> layerPath = path::join(staging, id, "rootfs");
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to untar image");
      } else if (!WIFEXITED(status.get()) || WEXITSTATUS(status.get()) != 0) {
        return Failure("Untar image failed with exit code: " +
                        WSTRINGIFY(status.get()));
      }

      return moveLayer(staging, id, sandbox);
    });
}


Future<Nothing> LocalStoreProcess::moveLayer(
    const string& staging,
    const string& id,
    const string& sandbox){

  Try<int> out = os::open(
      path::join(sandbox, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (out.isError()) {
    return Failure("Failed to create 'stdout' file: " + out.error());
  }

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(sandbox, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (err.isError()) {
    os::close(out.get());
    return Failure("Failed to create 'stderr' file: " + err.error());
  }

  if (!os::exists(flags.docker_store_dir)) {
    VLOG(1) << "Creating docker store directory";
    os::mkdir(flags.docker_store_dir);
  }

  if (!os::exists(path::join(flags.docker_store_dir, id))) {
    os::mkdir(path::join(flags.docker_store_dir, id));
  }

  Try<Nothing> status = os::rename(
      path::join(staging, id, "rootfs"),
      path::join(flags.docker_store_dir, id, "rootfs"));

  if (status.isError()) {
    return Failure("Failed to move layer to store directory:" + status.error());
  }

  return Nothing();
}


Future<Option<DockerImage>> LocalStoreProcess::get(const string& name)
{
  return refStore->get(name);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
