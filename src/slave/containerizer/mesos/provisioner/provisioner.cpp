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

#include <mesos/type_utils.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/backend.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"
#include "slave/containerizer/mesos/provisioner/store.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<Provisioner>> Provisioner::create(const Flags& flags)
{
  string _rootDir = slave::paths::getProvisionerDir(flags.work_dir);

  Try<Nothing> mkdir = os::mkdir(_rootDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create provisioner root directory '" +
        _rootDir + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(_rootDir);
  if (rootDir.isError()) {
    return Error(
        "Failed to resolve the realpath of provisioner root directory '" +
        _rootDir + "': " + rootDir.error());
  }

  CHECK_SOME(rootDir); // Can't be None since we just created it.

  Try<hashmap<Image::Type, Owned<Store>>> stores = Store::create(flags);
  if (stores.isError()) {
    return Error("Failed to create image stores: " + stores.error());
  }

  hashmap<string, Owned<Backend>> backends = Backend::create(flags);
  if (backends.empty()) {
    return Error("No usable provisioner backend created");
  }

  if (!backends.contains(flags.image_provisioner_backend)) {
    return Error(
        "The specified provisioner backend '" +
        flags.image_provisioner_backend + "' is unsupported");
  }

  return Owned<Provisioner>(new Provisioner(
      Owned<ProvisionerProcess>(new ProvisionerProcess(
          flags,
          rootDir.get(),
          stores.get(),
          backends))));
}


Provisioner::Provisioner(Owned<ProvisionerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Provisioner::~Provisioner()
{
  if (process.get() != NULL) {
    terminate(process.get());
    wait(process.get());
  }
}


Future<Nothing> Provisioner::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::recover,
      states,
      orphans);
}


Future<ProvisionInfo> Provisioner::provision(
    const ContainerID& containerId,
    const Image& image)
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::provision,
      containerId,
      image);
}


Future<bool> Provisioner::destroy(const ContainerID& containerId)
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::destroy,
      containerId);
}


ProvisionerProcess::ProvisionerProcess(
    const Flags& _flags,
    const string& _rootDir,
    const hashmap<Image::Type, Owned<Store>>& _stores,
    const hashmap<string, Owned<Backend>>& _backends)
  : flags(_flags),
    rootDir(_rootDir),
    stores(_stores),
    backends(_backends) {}


Future<Nothing> ProvisionerProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Register living containers, including the ones that do not
  // provision images.
  hashset<ContainerID> alive;
  foreach (const ContainerState& state, states) {
    alive.insert(state.container_id());
  }

  // List provisioned containers; recover living ones; destroy unknown
  // orphans. Note that known orphan containers are recovered as well
  // and they will be destroyed by the containerizer using the normal
  // cleanup path. See MESOS-2367 for details.
  Try<hashset<ContainerID>> containers =
    provisioner::paths::listContainers(rootDir);

  if (containers.isError()) {
    return Failure(
        "Failed to list the containers managed by the provisioner: " +
        containers.error());
  }

  // Scan the list of containers, register all of them with 'infos'
  // but mark unknown orphans for immediate cleanup.
  hashset<ContainerID> unknownOrphans;

  foreach (const ContainerID& containerId, containers.get()) {
    Owned<Info> info = Owned<Info>(new Info());

    Try<hashmap<string, hashset<string>>> rootfses =
      provisioner::paths::listContainerRootfses(rootDir, containerId);

    if (rootfses.isError()) {
      return Failure(
          "Unable to list rootfses belonged to container " +
          stringify(containerId) + ": " + rootfses.error());
    }

    foreachkey (const string& backend, rootfses.get()) {
      if (!backends.contains(backend)) {
        return Failure(
            "Found rootfses managed by an unrecognized backend: " + backend);
      }

      info->rootfses.put(backend, rootfses.get()[backend]);
    }

    infos.put(containerId, info);

    if (alive.contains(containerId) || orphans.contains(containerId)) {
      LOG(INFO) << "Recovered container " << containerId;
      continue;
    } else {
      // For immediate cleanup below.
      unknownOrphans.insert(containerId);
    }
  }

  // Cleanup unknown orphan containers' rootfses.
  list<Future<bool>> cleanups;
  foreach (const ContainerID& containerId, unknownOrphans) {
    LOG(INFO) << "Cleaning up unknown orphan container " << containerId;
    cleanups.push_back(destroy(containerId));
  }

  Future<Nothing> cleanup = collect(cleanups)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // Recover stores.
  list<Future<Nothing>> recovers;
  foreachvalue (const Owned<Store>& store, stores) {
    recovers.push_back(store->recover());
  }

  Future<Nothing> recover = collect(recovers)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // A successful provisioner recovery depends on:
  // 1) Recovery of living containers and known orphans (done above).
  // 2) Successful cleanup of unknown orphans.
  // 3) Successful store recovery.
  //
  // TODO(jieyu): Do not recover 'store' before unknown orphans are
  // cleaned up. In the future, we may want to cleanup unused rootfses
  // in 'store', which might fail if there still exist unknown orphans
  // holding references to them.
  return collect(cleanup, recover)
    .then([=]() -> Future<Nothing> {
      LOG(INFO) << "Provisioner recovery complete";
      return Nothing();
    });
}


Future<ProvisionInfo> ProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image)
{
  if (!stores.contains(image.type())) {
    return Failure(
        "Unsupported container image type: " +
        stringify(image.type()));
  }

  // Get and then provision image layers from the store.
  return stores.get(image.type()).get()->get(image)
    .then(defer(self(), &Self::_provision, containerId, lambda::_1));
}


Future<ProvisionInfo> ProvisionerProcess::_provision(
    const ContainerID& containerId,
    const ImageInfo& ImageInfo)
{
  // TODO(jieyu): Choose a backend smartly. For instance, if there is
  // only one layer returned from the store. prefer to use bind
  // backend because it's the simplest.
  const string& backend = flags.image_provisioner_backend;
  CHECK(backends.contains(backend));

  string rootfsId = UUID::random().toString();

  string rootfs = provisioner::paths::getContainerRootfsDir(
      rootDir,
      containerId,
      backend,
      rootfsId);

  LOG(INFO) << "Provisioning image rootfs '" << rootfs
            << "' for container " << containerId;

  // NOTE: It's likely that the container ID already exists in 'infos'
  // because one container might provision multiple images.
  if (!infos.contains(containerId)) {
    infos.put(containerId, Owned<Info>(new Info()));
  }

  infos[containerId]->rootfses[backend].insert(rootfsId);

  return backends.get(backend).get()->provision(ImageInfo.layers, rootfs)
    .then([rootfs, ImageInfo]() -> Future<ProvisionInfo> {
      return ProvisionInfo{rootfs, ImageInfo.runtimeConfig};
    });
}


Future<bool> ProvisionerProcess::destroy(const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring destroy request for unknown container "
              << containerId;

    return false;
  }

  // Unregister the container first. If destroy() fails, we can rely
  // on recover() to retry it later.
  Owned<Info> info = infos[containerId];
  infos.erase(containerId);

  list<Future<bool>> futures;
  foreachkey (const string& backend, info->rootfses) {
    if (!backends.contains(backend)) {
      return Failure("Unknown backend '" + backend + "'");
    }

    foreach (const string& rootfsId, info->rootfses[backend]) {
      string rootfs = provisioner::paths::getContainerRootfsDir(
          rootDir,
          containerId,
          backend,
          rootfsId);

      LOG(INFO) << "Destroying container rootfs at '" << rootfs
                << "' for container " << containerId;

      futures.push_back(backends.get(backend).get()->destroy(rootfs));
    }
  }

  // TODO(xujyan): Revisit the usefulness of this return value.
  return collect(futures)
    .then(defer(self(), &ProvisionerProcess::_destroy, containerId));
}


Future<bool> ProvisionerProcess::_destroy(const ContainerID& containerId)
{
  // This should be fairly cheap as the directory should only
  // contain a few empty sub-directories at this point.
  //
  // TODO(jieyu): Currently, it's possible that some directories
  // cannot be removed due to EBUSY. EBUSY is caused by the race
  // between cleaning up this container and new containers copying
  // the host mount table. It's OK to ignore them. The cleanup
  // will be retried during slave recovery.
  string containerDir =
    provisioner::paths::getContainerDir(rootDir, containerId);

  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    LOG(ERROR) << "Failed to remove the provisioned container directory "
               << "at '" << containerDir << "': " << rmdir.error();

    ++metrics.remove_container_errors;
  }

  return true;
}


ProvisionerProcess::Metrics::Metrics()
  : remove_container_errors(
      "containerizer/mesos/provisioner/remove_container_errors")
{
  process::metrics::add(remove_container_errors);
}


ProvisionerProcess::Metrics::~Metrics()
{
  process::metrics::remove(remove_container_errors);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
