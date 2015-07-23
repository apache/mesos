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

#include "linux/fs.hpp"

#include "slave/containerizer/provisioners/docker/store.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

ImageName::ImageName(const std::string& name)
{
  registry = None();
  std::vector<std::string> components = strings::split(name, "/");
  if (components.size() > 2) {
    registry = name.substr(0, name.find_last_of("/"));
  }

  std::size_t found = components.back().find_last_of(':');
  if (found == std::string::npos) {
    repo = components.back();
    tag = "latest";
  } else {
    repo = components.back().substr(0, found);
    tag = components.back().substr(found + 1);
  }
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
    const Image& image,
    const std::string& sandbox)
{
  return dispatch(
      process.get(),
      &DockerProvisionerProcess::provision,
      containerId,
      image,
      sandbox);
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
  Try<Nothing> mkdir = os::mkdir(flags.docker_rootfs_dir);
  if (mkdir.isError()) {
    return Error("Failed to create provisioner rootfs directory '" +
                 flags.docker_rootfs_dir + "': " + mkdir.error());
  }

  Try<Owned<Store>> store = Store::create(flags, fetcher);
  if (store.isError()) {
    return Error("Failed to create image store: " + store.error());
  }

  hashmap<string, Owned<mesos::internal::slave::Backend>> backendOptions =
    mesos::internal::slave::Backend::create(flags);

  return Owned<DockerProvisionerProcess>(
      new DockerProvisionerProcess(
          flags,
          store.get(),
          backendOptions[flags.docker_backend]));
}


DockerProvisionerProcess::DockerProvisionerProcess(
    const Flags& _flags,
    const Owned<Store>& _store,
    const Owned<mesos::internal::slave::Backend>& _backend)
  : flags(_flags),
    store(_store),
    backend(_backend) {}


Future<Nothing> DockerProvisionerProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<string> DockerProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image,
    const string& sandbox)
{
  if (image.type() != Image::DOCKER) {
    return Failure("Unsupported container image type");
  }

  if (!image.has_docker()) {
    return Failure("Missing Docker image info");
  }

  return fetch(image.docker().name(), sandbox)
    .then(defer(self(),
                &Self::_provision,
                containerId,
                lambda::_1));
}


Future<string> DockerProvisionerProcess::_provision(
    const ContainerID& containerId,
    const DockerImage& image)
{
  // Create root directory.
  string base = path::join(flags.docker_rootfs_dir,
                           stringify(containerId));

  string rootfs = path::join(base, "rootfs");

  Try<Nothing> mkdir = os::mkdir(base);
  if (mkdir.isError()) {
    return Failure("Failed to create directory for container filesystem: " +
                    mkdir.error());
  }

  LOG(INFO) << "Provisioning rootfs for container '" << containerId << "'"
            << " to '" << base << "'";

  vector<string> layerPaths;
  foreach (const string& layerId, image.layers) {
    layerPaths.push_back(path::join(flags.docker_store_dir, layerId, "rootfs"));
  }

  return backend->provision(layerPaths, base)
    .then([rootfs]() -> Future<string> {
      // Bind mount the rootfs to itself so we can pivot_root. We do
      // it now so any subsequent mounts by the containerizer or
      // isolators are correctly handled by pivot_root.
      Try<Nothing> mount =
        fs::mount(rootfs, rootfs, None(), MS_BIND | MS_SHARED, NULL);
      if (mount.isError()) {
        return Failure("Failure to bind mount rootfs: " + mount.error());
      }

      return rootfs;
    });
}


// Fetch an image and all dependencies.
Future<DockerImage> DockerProvisionerProcess::fetch(
    const string& name,
    const string& sandbox)
{
  return store->get(name)
    .then([=](const Option<DockerImage>& image) -> Future<DockerImage> {
      if (image.isSome()) {
        return image.get();
      }
      return store->put(name, sandbox);
    });
}


Future<bool> DockerProvisionerProcess::destroy(
    const ContainerID& containerId)
{
  string base = path::join(flags.docker_rootfs_dir, stringify(containerId));

  if (!os::exists(base)) {
    return false;
  }

  LOG(INFO) << "Destroying container rootfs for container '"
            << containerId << "'";

  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
  if (mountTable.isError()) {
    return Failure("Failed to read mount table: " + mountTable.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, mountTable.get().entries) {
    if (strings::startsWith(entry.target, base)) {
      Try<Nothing> unmount = fs::unmount(entry.target, MNT_DETACH);
      if (unmount.isError()) {
        return Failure("Failed to unmount mount table target: " +
                        unmount.error());
      }
    }
  }

  return backend->destroy(base);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
