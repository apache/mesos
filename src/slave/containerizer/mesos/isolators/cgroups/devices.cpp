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

#include <stdint.h>

#include <list>
#include <set>
#include <string>
#include <vector>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/devices.hpp"

using cgroups::devices::Entry;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::internal::slave::Containerizer;

using process::Failure;
using process::Future;
using process::PID;
using process::Shared;

using std::list;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// The default list of devices to whitelist when device isolation is
// turned on. The full list of devices can be found here:
// https://www.kernel.org/doc/Documentation/devices.txt
//
// Device whitelisting is described here:
// https://www.kernel.org/doc/Documentation/cgroup-v1/devices.txt
static const char* DEFAULT_WHITELIST_ENTRIES[] = {
  "c *:* m",      // Make new character devices.
  "b *:* m",      // Make new block devices.
  "c 5:1 rwm",    // /dev/console
  "c 4:0 rwm",    // /dev/tty0
  "c 4:1 rwm",    // /dev/tty1
  "c 136:* rwm",  // /dev/pts/*
  "c 5:2 rwm",    // /dev/ptmx
  "c 10:200 rwm", // /dev/net/tun
  "c 1:3 rwm",    // /dev/null
  "c 1:5 rwm",    // /dev/zero
  "c 1:7 rwm",    // /dev/full
  "c 5:0 rwm",    // /dev/tty
  "c 1:9 rwm",    // /dev/urandom
  "c 1:8 rwm",    // /dev/random
};


CgroupsDevicesIsolatorProcess::CgroupsDevicesIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : ProcessBase(process::ID::generate("cgroups-devices-isolator")),
    flags(_flags),
    hierarchy(_hierarchy) {}


Try<Isolator*> CgroupsDevicesIsolatorProcess::create(const Flags& flags)
{
  // Prepare the cgroups device hierarchy.
  Try<string> hierarchy = cgroups::prepare(
        flags.cgroups_hierarchy,
        "devices",
        flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to prepare hierarchy for 'devices' subsystem: " +
                 hierarchy.error());
  }

  // Ensure that no other subsystem is attached to the hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(hierarchy.get());
  if (subsystems.isError()) {
    return Error("Failed to get subsystems attached to hierarchy"
                 " '" + stringify(hierarchy.get()) + "': " +
                 subsystems.error());
  } else if (subsystems->size() != 1) {
    return Error("Unexpected subsystems attached to hierarchy"
                 " '" + stringify(hierarchy.get()) + "': " +
                 stringify(subsystems.get()));
  }

  process::Owned<MesosIsolatorProcess> process(
      new CgroupsDevicesIsolatorProcess(
          flags,
          hierarchy.get()));

  return new MesosIsolator(process);
}


Future<Nothing> CgroupsDevicesIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("Failed to check cgroup for container '" +
                     stringify(containerId) + "'");
    }

    if (!exists.get()) {
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);
  }

  // Remove orphan cgroups.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
    foreachvalue (Info* info, infos) {
      delete info;
    }
    infos.clear();
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Ignore the slave cgroup (see the --agent_subsystems flag).
    // TODO(idownes): Remove this when the cgroups layout is updated,
    // see MESOS-1185.
    if (cgroup == path::join(flags.cgroups_root, "slave")) {
      continue;
    }

    ContainerID containerId;
    containerId.set_value(Path(cgroup).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    // Known orphan cgroups will be destroyed by the containerizer
    // using the normal cleanup path. See MESOS-2367 for details.
    if (orphans.contains(containerId)) {
      infos[containerId] = new Info(containerId, cgroup);
      continue;
    }

    LOG(INFO) << "Removing unknown orphaned cgroup '" << cgroup << "'";

    // We don't wait on the destroy as we don't want to block recovery.
    cgroups::destroy(hierarchy, cgroup, cgroups::DESTROY_TIMEOUT);
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> CgroupsDevicesIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // TODO(bmahler): Don't insert into 'infos' unless we create the
  // cgroup successfully. It's safe for now because 'cleanup' gets
  // called if we return a Failure, but cleanup will fail because the
  // cgroup does not exist when cgroups::destroy is called.
  Info* info = new Info(
      containerId, path::join(flags.cgroups_root, containerId.value()));

  infos[containerId] = info;

  Try<bool> exists = cgroups::exists(hierarchy, info->cgroup);
  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  } else if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  Try<Nothing> create = cgroups::create(hierarchy, info->cgroup);
  if (create.isError()) {
    return Failure("Failed to prepare isolator: " + create.error());
  }

  // Chown the cgroup so the executor can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  if (containerConfig.has_user()) {
    Try<Nothing> chown = os::chown(
        containerConfig.user(),
        path::join(hierarchy, info->cgroup),
        false);
    if (chown.isError()) {
      return Failure("Failed to prepare isolator: " + chown.error());
    }
  }

  // When a devices cgroup is first created, its whitelist inherits
  // all devices from its parent's whitelist (i.e., "a *:* rwm" by
  // default). In theory, we should be able to add and remove devices
  // from the whitelist by writing to the respective `devices.allow`
  // and `devices.deny` files associated with the cgroup. However, the
  // semantics of the whitelist are such that writing to the deny file
  // will only remove entries in the whitelist that are explicitly
  // listed in there (i.e., denying "b 1:3 rwm" when the whitelist
  // only contains "a *:* rwm" will not modify the whitelist because
  // "b 1:3 rwm" is not explicitly listed). Although the whitelist
  // doesn't change, access to the device is still denied as expected
  // (there is just no way of querying the system to detect it).
  // Because of this, we first deny access to all devices and
  // selectively add some back in so we can control the entries in the
  // whitelist explicitly.
  cgroups::devices::Entry all;
  all.selector.type = Entry::Selector::Type::ALL;
  all.selector.major = None();
  all.selector.minor = None();
  all.access.read = true;
  all.access.write = true;
  all.access.mknod = true;

  Try<Nothing> deny = cgroups::devices::deny(hierarchy, info->cgroup, all);

  if (deny.isError()) {
    return Failure("Failed to deny all devices: " + deny.error());
  }

  foreach (const char* _entry, DEFAULT_WHITELIST_ENTRIES) {
    Try<cgroups::devices::Entry> entry =
      cgroups::devices::Entry::parse(_entry);

    CHECK_SOME(entry);

    Try<Nothing> allow =
      cgroups::devices::allow(hierarchy, info->cgroup, entry.get());

    if (allow.isError()) {
      return Failure("Failed to whitelist default device"
                     " '" + stringify(entry.get()) + "': " + allow.error());
    }
  }

  return None();
}


Future<Nothing> CgroupsDevicesIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  Try<Nothing> assign = cgroups::assign(
      hierarchy, info->cgroup, pid);

  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign container '" << info->containerId << "'"
               << " to cgroup '" << path::join(hierarchy, info->cgroup) << "': "
               << assign.error();

    return Failure("Failed to isolate container: " + assign.error());
  }

  return Nothing();
}


Future<Nothing> CgroupsDevicesIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  return cgroups::destroy(hierarchy, info->cgroup, cgroups::DESTROY_TIMEOUT)
    .onAny(defer(PID<CgroupsDevicesIsolatorProcess>(this),
                 &CgroupsDevicesIsolatorProcess::_cleanup,
                 containerId,
                 lambda::_1));
}


Future<Nothing> CgroupsDevicesIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (!future.isReady()) {
    return Failure(
        "Failed to clean up container " + stringify(containerId) + ": " +
        (future.isFailed() ? future.failure() : "discarded"));
  }

  delete info;
  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
