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

#include <mesos/type_utils.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "slave/containerizer/provisioners/backend.hpp"
#include "slave/containerizer/provisioners/paths.hpp"

#include "slave/containerizer/provisioners/appc/paths.hpp"
#include "slave/containerizer/provisioners/appc/provisioner.hpp"
#include "slave/containerizer/provisioners/appc/spec.hpp"
#include "slave/containerizer/provisioners/appc/store.hpp"

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
namespace appc {

class AppcProvisionerProcess : public Process<AppcProvisionerProcess>
{
public:
  AppcProvisionerProcess(
      const Flags& flags,
      const string& root,
      const Owned<Store>& store,
      const hashmap<string, Owned<Backend>>& backends);

  Future<Nothing> recover(
      const list<ContainerState>& states,
      const hashset<ContainerID>& orphans);

  Future<string> provision(const ContainerID& containerId, const Image& image);

  Future<bool> destroy(const ContainerID& containerId);

private:
  Future<string> _provision(const vector<string>& layers, const string& rootfs);

  const Flags flags;

  // Absolute path to the Appc provisioner root directory. It can be derived
  // from '--work_dir' but we keep a separate copy here because we converted
  // it into an absolute path so managed rootfs paths match the ones in
  // 'mountinfo' (important if mount-based backends are used).
  const string root;

  const Owned<Store> store;
  const hashmap<string, Owned<Backend>> backends;

  struct Info
  {
    // Mappings: backend -> rootfsId -> rootfsPath.
    hashmap<string, hashmap<string, string>> rootfses;
  };

  hashmap<ContainerID, Owned<Info>> infos;
};


// NOTE: Successful creation of the provisioner means its managed
// directory under --work_dir is also created.
Try<Owned<Provisioner>> AppcProvisioner::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  string _root =
    slave::paths::getProvisionerDir(flags.work_dir, Image::APPC);

  Try<Nothing> mkdir = os::mkdir(_root);
  if (mkdir.isError()) {
    return Error("Failed to create provisioner root directory '" +
                 _root + "': " + mkdir.error());
  }

  Result<string> root = os::realpath(_root);
  if (root.isError()) {
    return Error(
        "Failed to resolve the realpath of provisioner root directory '" +
        _root + "': " + root.error());
  }

  CHECK_SOME(root); // Can't be None since we just created it.

  Try<Owned<Store>> store = Store::create(flags);
  if (store.isError()) {
    return Error("Failed to create image store: " + store.error());
  }

  hashmap<string, Owned<Backend>> backends = Backend::create(flags);
  if (backends.empty()) {
    return Error("No usable provisioner backend created");
  }

  if (!backends.contains(flags.appc_provisioner_backend)) {
    return Error("The specified provisioner backend '" +
                 flags.appc_provisioner_backend + "'is unsupported");
  }

  return Owned<Provisioner>(new AppcProvisioner(
      Owned<AppcProvisionerProcess>(new AppcProvisionerProcess(
          flags,
          root.get(),
          store.get(),
          backends))));
}


AppcProvisioner::AppcProvisioner(Owned<AppcProvisionerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


AppcProvisioner::~AppcProvisioner()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> AppcProvisioner::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return dispatch(
      process.get(),
      &AppcProvisionerProcess::recover,
      states,
      orphans);
}


Future<string> AppcProvisioner::provision(
    const ContainerID& containerId,
    const Image& image)
{
  return dispatch(
      process.get(),
      &AppcProvisionerProcess::provision,
      containerId,
      image);
}


Future<bool> AppcProvisioner::destroy(const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &AppcProvisionerProcess::destroy,
      containerId);
}


AppcProvisionerProcess::AppcProvisionerProcess(
    const Flags& _flags,
    const string& _root,
    const Owned<Store>& _store,
    const hashmap<string, Owned<Backend>>& _backends)
  : flags(_flags),
    root(_root),
    store(_store),
    backends(_backends) {}


Future<Nothing> AppcProvisionerProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Register living containers, including the ones that do not
  // provision Appc images.
  hashset<ContainerID> alive;
  foreach (const ContainerState& state, states) {
    alive.insert(state.container_id());
  }

  // List provisioned containers; recover living ones; destroy unknown orphans.
  // Note that known orphan containers are recovered as well and they will
  // be destroyed by the containerizer using the normal cleanup path. See
  // MESOS-2367 for details.
  Try<hashmap<ContainerID, string>> containers =
    provisioners::paths::listContainers(root);

  if (containers.isError()) {
    return Failure("Failed to list the containers managed by Appc "
                   "provisioner: " + containers.error());
  }

  // Scan the list of containers, register all of them with 'infos' but
  // mark unknown orphans for immediate cleanup.
  hashset<ContainerID> unknownOrphans;
  foreachkey (const ContainerID& containerId, containers.get()) {
    Owned<Info> info = Owned<Info>(new Info());

    Try<hashmap<string, hashmap<string, string>>> rootfses =
      provisioners::paths::listContainerRootfses(root, containerId);

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

    infos.put(containerId, info);

    if (alive.contains(containerId) || orphans.contains(containerId)) {
      VLOG(1) << "Recovered container " << containerId;
      continue;
    } else {
      // For immediate cleanup below.
      unknownOrphans.insert(containerId);
    }
  }

  LOG(INFO)
    << "Recovered living and known orphan containers for Appc provisioner";

  // Destroy unknown orphan containers' rootfses.
  list<Future<bool>> destroys;
  foreach (const ContainerID& containerId, unknownOrphans) {
    destroys.push_back(destroy(containerId));
  }

  Future<Nothing> cleanup = collect(destroys)
    .then([]() -> Future<Nothing> {
      LOG(INFO) << "Cleaned up unknown orphan containers for Appc provisioner";
      return Nothing();
    });

  Future<Nothing> recover = store->recover()
    .then([]() -> Future<Nothing> {
      LOG(INFO) << "Recovered Appc image store";
      return Nothing();
    });


  // A successful provisioner recovery depends on:
  // 1) Recovery of living containers and known orphans (done above).
  // 2) Successful cleanup of unknown orphans.
  // 3) Successful store recovery.
  return collect(cleanup, recover)
    .then([=]() -> Future<Nothing> {
      return Nothing();
    });
}


Future<string> AppcProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image)
{
  if (image.type() != Image::APPC) {
    return Failure("Unsupported container image type: " +
                   stringify(image.type()));
  }

  if (!image.has_appc()) {
    return Failure("Missing Appc image info");
  }

  string rootfsId = UUID::random().toString();
  string rootfs = provisioners::paths::getContainerRootfsDir(
      root, containerId, flags.appc_provisioner_backend, rootfsId);

  if (!infos.contains(containerId)) {
    infos.put(containerId, Owned<Info>(new Info()));
  }

  infos[containerId]->rootfses[flags.appc_provisioner_backend].put(
      rootfsId, rootfs);

  // Get and then provision image layers from the store.
  return store->get(image.appc())
    .then(defer(self(), &Self::_provision, lambda::_1, rootfs));
}


Future<string> AppcProvisionerProcess::_provision(
     const vector<string>& layers,
     const string& rootfs)
{
  LOG(INFO) << "Provisioning image layers to rootfs '" << rootfs << "'";

  CHECK(backends.contains(flags.appc_provisioner_backend));
  return backends.get(flags.appc_provisioner_backend).get()->provision(
      layers,
      rootfs)
    .then([rootfs]() -> Future<string> { return rootfs; });
}


Future<bool> AppcProvisionerProcess::destroy(const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring destroy request for unknown container: "
              << containerId;

    return false;
  }

  // Unregister the container first. If destroy() fails, we can rely
  // on recover() to retry it later.
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

  // NOTE: We calculate 'containerDir' here so that the following
  // lambda does not need to bind 'this'.
  string containerDir =
    provisioners::paths::getContainerDir(root, containerId);

  // TODO(xujyan): Revisit the usefulness of this return value.
  return collect(futures)
    .then([containerDir]() -> Future<bool> {
      // This should be fairly cheap as the directory should only
      // contain a few empty sub-directories at this point.
      //
      // TODO(jieyu): Currently, it's possible that some directories
      // cannot be removed due to EBUSY. EBUSY is caused by the race
      // between cleaning up this container and new containers copying
      // the host mount table. It's OK to ignore them. The cleanup
      // will be retried during slave recovery.
      Try<Nothing> rmdir = os::rmdir(containerDir);
      if (rmdir.isError()) {
        LOG(ERROR) << "Failed to remove the provisioned container directory "
                   << "at '" << containerDir << "'";
      }

      return true;
    });
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
