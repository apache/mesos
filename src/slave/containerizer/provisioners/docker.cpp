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

#include "slave/containerizer/provisioners/docker.hpp"

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/owned.hpp>
#include <process/sequence.hpp>

#include "slave/containerizer/provisioners/backend.hpp"
#include "slave/containerizer/provisioners/paths.hpp"

#include "slave/containerizer/provisioners/docker/paths.hpp"
#include "slave/containerizer/provisioners/docker/store.hpp"

#include "slave/paths.hpp"

using namespace process;
using namespace mesos::internal::slave;

using std::list;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class DockerProvisionerProcess :
  public process::Process<DockerProvisionerProcess>
{
public:
  static Try<process::Owned<DockerProvisionerProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image);

  process::Future<bool> destroy(const ContainerID& containerId);

private:
  DockerProvisionerProcess(
      const string& _rootDir,
      const Flags& _flags,
      const process::Owned<Store>& _store,
      const hashmap<std::string, process::Owned<Backend>>& _backends);

  process::Future<std::string> _provision(
      const DockerImage& image,
      const ContainerID& containerId,
      const string& rootfs);

  const string& rootDir;
  const Flags flags;
  process::Owned<Store> store;
  hashmap<string, process::Owned<Backend>> backends;

  struct Info
  {
    // Mappings: backend -> rootfsId -> rootfsPath.
    hashmap<string, hashmap<string, string>> rootfses;
  };

  hashmap<ContainerID, Owned<Info>> infos;
};


Try<ImageName> ImageName::create(const std::string& name)
{
  ImageName imageName;
  Option<string> registry = None();
  std::vector<std::string> components = strings::split(name, "/");
  if (components.size() > 2) {
    registry = name.substr(0, name.find_last_of("/"));
  }

  std::size_t found = components.back().find_last_of(':');
  if (found == std::string::npos) {
    imageName.repository = components.back();
    imageName.tag = "latest";
  } else {
    imageName.repository = components.back().substr(0, found);
    imageName.tag = components.back().substr(found + 1);
  }

  return imageName;
}

Try<Owned<Provisioner>> DockerProvisioner::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Try<Owned<DockerProvisionerProcess>> create =
    DockerProvisionerProcess::create(flags, fetcher);
  if (create.isError()) {
    return Error(create.error());
  }

  return Owned<Provisioner>(new DockerProvisioner(create.get()));
}


DockerProvisioner::DockerProvisioner(Owned<DockerProvisionerProcess> _process)
  : process(_process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


DockerProvisioner::~DockerProvisioner()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> DockerProvisioner::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return dispatch(
      process.get(),
      &DockerProvisionerProcess::recover,
      states,
      orphans);
}


Future<string> DockerProvisioner::provision(
    const ContainerID& containerId,
    const Image& image)
{
  return dispatch(
      process.get(),
      &DockerProvisionerProcess::provision,
      containerId,
      image);
}


Future<bool> DockerProvisioner::destroy(const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerProvisionerProcess::destroy,
      containerId);
}


Try<Owned<DockerProvisionerProcess>> DockerProvisionerProcess::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  string rootDir =
    slave::paths::getProvisionerDir(flags.work_dir, Image::DOCKER);

  if (!os::exists(rootDir)) {
    Try<Nothing> mkdir = os::mkdir(rootDir);
    if (mkdir.isError()) {
      return Error("Failed to create Docker provisioner root directory '" +
                   rootDir + "': " + mkdir.error());
    }
  }

  const hashmap<string, Owned<Backend>> backends = Backend::create(flags);
  if (backends.empty()) {
    return Error("No usable Docker provisioner backend created");
  }

  if (!backends.contains(flags.docker_backend)) {
    return Error("The specified Docker provisioner backend '" +
                 flags.docker_backend + "'is unsupported");
  }

  Try<Owned<Store>> store = Store::create(flags, fetcher);
  if (store.isError()) {
    return Error("Failed to create image store: " + store.error());
  }

  return Owned<DockerProvisionerProcess>(
      new DockerProvisionerProcess(
          rootDir,
          flags,
          store.get(),
          backends));
}


DockerProvisionerProcess::DockerProvisionerProcess(
    const string& _rootDir,
    const Flags& _flags,
    const Owned<Store>& _store,
    const hashmap<string, Owned<Backend>>& _backends)
  : rootDir(_rootDir),
    flags(_flags),
    store(_store),
    backends(_backends) {}


Future<Nothing> DockerProvisionerProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // TODO(tnachen): Consider merging this with
  // AppcProvisionerProcess::recover.

  // Register living containers, including the ones that do not
  // provision Docker images.
  hashset<ContainerID> alive;

  foreach (const ContainerState& state, states) {
    if (state.executor_info().has_container() &&
        state.executor_info().container().type() == ContainerInfo::MESOS) {
      alive.insert(state.container_id());
    }
  }

  // List provisioned containers; recover living ones; destroy unknown orphans.
  // Note that known orphan containers are recovered as well and they will
  // be destroyed by the containerizer using the normal cleanup path. See
  // MESOS-2367 for details.
  Try<hashmap<ContainerID, string>> containers =
    provisioners::paths::listContainers(rootDir);

  if (containers.isError()) {
    return Failure("Failed to list the containers managed by Docker "
                   "provisioner: " + containers.error());
  }

  // If no container has been launched the 'containers' directory will be empty.
  foreachkey (const ContainerID& containerId, containers.get()) {
    if (alive.contains(containerId) || orphans.contains(containerId)) {
      Owned<Info> info = Owned<Info>(new Info());

      Try<hashmap<string, hashmap<string, string>>> rootfses =
        provisioners::paths::listContainerRootfses(rootDir, containerId);

      if (rootfses.isError()) {
        return Failure("Unable to list rootfses belonged to container '" +
                       containerId.value() + "': " + rootfses.error());
      }

      foreachkey (const string& backend, rootfses.get()) {
        if (!backends.contains(backend)) {
          return Failure("Found rootfses managed by an unrecognized backend: " +
                         backend);
        }

        info->rootfses.put(backend, rootfses.get()[backend]);
      }

      VLOG(1) << "Recovered container " << containerId;
      infos.put(containerId, info);

      continue;
    }

    // Destroy (unknown) orphan container's rootfses.
    Try<hashmap<string, hashmap<string, string>>> rootfses =
      provisioners::paths::listContainerRootfses(rootDir, containerId);

    if (rootfses.isError()) {
      return Failure("Unable to find rootfses for container '" +
                     containerId.value() + "': " + rootfses.error());
    }

    foreachkey (const string& backend, rootfses.get()) {
      if (!backends.contains(backend)) {
        return Failure("Found rootfses managed by an unrecognized backend: " +
                       backend);
      }

      foreachvalue (const string& rootfs, rootfses.get()[backend]) {
        VLOG(1) << "Destroying orphan rootfs " << rootfs;

        // Not waiting for the destruction and we don't care about
        // the return value.
        backends.get(backend).get()->destroy(rootfs)
          .onFailed([rootfs](const std::string& error) {
            LOG(WARNING) << "Failed to destroy orphan rootfs '" << rootfs
                         << "': "<< error;
          });
      }
    }
  }

  LOG(INFO) << "Recovered Docker provisioner rootfses";

  return store->recover()
    .then([]() -> Future<Nothing> {
      LOG(INFO) << "Recovered Docker image store";
      return Nothing();
    });
}


Future<string> DockerProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image)
{
  if (image.type() != Image::DOCKER) {
    return Failure("Unsupported container image type");
  }

  if (!image.has_docker()) {
    return Failure("Missing Docker image info");
  }

  string rootfsId = UUID::random().toString();
  string rootfs = provisioners::paths::getContainerRootfsDir(
      rootDir, containerId, flags.docker_backend, rootfsId);

  if (!infos.contains(containerId)) {
    infos.put(containerId, Owned<Info>(new Info()));
  }

  infos[containerId]->rootfses[flags.docker_backend].put(rootfsId, rootfs);

  Try<ImageName> imageName = ImageName::create(image.docker().name());
  if (imageName.isError()) {
    return Failure("Unable to able to parse Docker image name '" +
                   image.docker().name() + "': " + imageName.error());
  }

  return store->get(imageName.get())
    .then(defer(self(), &Self::_provision, lambda::_1, containerId, rootfs));
}


Future<string> DockerProvisionerProcess::_provision(
    const DockerImage& image,
    const ContainerID& containerId,
    const string& rootfs)
{
  CHECK(backends.contains(flags.docker_backend));

  LOG(INFO) << "Provisioning rootfs for container '" << containerId << "'"
            << " to '" << rootfs << "'";

  vector<string> layerPaths;
  foreach (const string& layerId, image.layerIds) {
    layerPaths.push_back(
        paths::getImageLayerRootfsPath(flags.docker_store_dir, layerId));
  }

  return backends[flags.docker_backend]->provision(layerPaths, rootfs)
    .then([rootfs]() -> Future<string> { return rootfs; });
}


Future<bool> DockerProvisionerProcess::destroy(
    const ContainerID& containerId)
{
  // TODO(tnachen): Consider merging this with
  // AppcProvisionerProcess::destroy.
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring destroy request for unknown container: "
              << containerId;

    return false;
  }

  // Unregister the container first. If destroy() fails, we can rely on
  // recover() to retry it later.
  Owned<Info> info = infos[containerId];
  infos.erase(containerId);

  list<Future<bool>> futures;
  foreachkey (const string& backend, info->rootfses) {
    foreachvalue (const string& rootfs, info->rootfses[backend]) {
      if (!backends.contains(backend)) {
        return Failure("Cannot destroy rootfs '" + rootfs +
                       "' provisioned by an unknown backend '" + backend + "'");
      }

      LOG(INFO) << "Destroying container rootfs for container '"
                << containerId << "' at '" << rootfs << "'";

      futures.push_back(
          backends.get(backend).get()->destroy(rootfs));
    }
  }

  return collect(futures)
    .then([=](const list<bool>& results) -> Future<bool> {
      return true;
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
