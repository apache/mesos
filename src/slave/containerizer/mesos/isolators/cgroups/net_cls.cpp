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
#include <sstream>
#include <vector>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/net_cls.hpp"

using std::bitset;
using std::list;
using std::ostream;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> CgroupsNetClsIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "net_cls",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create net_cls cgroup: " + hierarchy.error());
  }

  // Ensure that no unexpected subsystem is attached to the hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(hierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy "
        "'" + hierarchy.get() + "'");
  } else if (subsystems.get().size() != 1) {
    // Some Linux distributions mount net_cls and net_prio subsystems
    // to the same hierarchy.
    // TODO(jieyu): If we ever introduce a cgroups net_prio isolator,
    // we need to make sure it will not conflict with this isolator if
    // two subsystems are co-mounted into the same hierarchy. For
    // instance, we should not remove a cgroup twice.
    foreach (const string& subsystem, subsystems.get()) {
      if (subsystem != "net_cls" && subsystem != "net_prio") {
        return Error(
            "Unexpected subsystems found attached to hierarchy "
            "'" + hierarchy.get() + "'");
      }
    }
  }

  IntervalSet<uint32_t> primaries;
  IntervalSet<uint32_t> secondaries;

  // Primary handle.
  if (flags.cgroups_net_cls_primary_handle.isSome()) {
    Try<uint16_t> primary = numify<uint16_t>(
        flags.cgroups_net_cls_primary_handle.get());

    if (primary.isError()) {
      return Error(
          "Failed to parse the primary handle '" +
          flags.cgroups_net_cls_primary_handle.get() +
          "' set in flag --cgroups_net_cls_primary_handle");
    }

    primaries +=
      (Bound<uint32_t>::closed(primary.get()),
       Bound<uint32_t>::closed(primary.get()));

    // Range of valid secondary handles.
    if (flags.cgroups_net_cls_secondary_handles.isSome()) {
      vector<string> range =
        strings::tokenize(flags.cgroups_net_cls_secondary_handles.get(), ",");

      if (range.size() != 2) {
        return Error(
            "Failed to parse the range of secondary handles " +
            flags.cgroups_net_cls_secondary_handles.get() +
            " set in flag --cgroups_net_cls_secondary_handles");
      }

      Try<uint16_t> lower = numify<uint16_t>(range[0]);
      if (lower.isError()) {
        return Error(
            "Failed to parse the lower bound of range of secondary handles" +
            flags.cgroups_net_cls_secondary_handles.get() +
            " set in flag --cgroups_net_cls_secondary_handles");
      }

      if (lower.get() == 0) {
        return Error("The secondary handle has to be a non-zero value.");
      }

      Try<uint16_t> upper =  numify<uint16_t>(range[1]);
      if (upper.isError()) {
        return Error(
            "Failed to parse the upper bound of range of secondary handles" +
            flags.cgroups_net_cls_secondary_handles.get() +
            " set in flag --cgroups_net_cls_secondary_handles");
      }

      secondaries +=
        (Bound<uint32_t>::closed(lower.get()),
         Bound<uint32_t>::closed(upper.get()));

      if (secondaries.empty()) {
        return Error(
            "Secondary handle range specified " +
            flags.cgroups_net_cls_secondary_handles.get() +
            ", in flag --cgroups_net_cls_secondary_handles, is an empty set");
      }
    }
  }

  process::Owned<MesosIsolatorProcess> process(
      new CgroupsNetClsIsolatorProcess(
          flags,
          hierarchy.get(),
          primaries,
          secondaries));

  return new MesosIsolator(process);
}


CgroupsNetClsIsolatorProcess::CgroupsNetClsIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy,
    const IntervalSet<uint32_t>& primaries,
    const IntervalSet<uint32_t>& secondaries)
  : ProcessBase(process::ID::generate("cgroups-net-cls-isolator")),
    flags(_flags),
    hierarchy(_hierarchy)
{
  if (!primaries.empty()) {
    handleManager = NetClsHandleManager(primaries, secondaries);
  }
}


CgroupsNetClsIsolatorProcess::~CgroupsNetClsIsolatorProcess() {}


Future<Nothing> CgroupsNetClsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      infos.clear();
      return Failure(
          "Failed to check cgroup for container " + stringify(containerId));
    }

    if (!exists.get()) {
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      continue;
    }

    // Read the net_cls handle.
    Result<NetClsHandle> handle = recoverHandle(hierarchy, cgroup);
    if (handle.isError()) {
      infos.clear();
      return Failure(
          "Failed to recover the net_cls handle for " +
          stringify(containerId) + ": " + handle.error());
    }

    if (handle.isSome()) {
      infos.emplace(containerId, Info(cgroup, handle.get()));
    } else {
      infos.emplace(containerId, Info(cgroup));
    }
  }

  // Remove orphan cgroups.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
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

    Result<NetClsHandle> handle = recoverHandle(hierarchy, cgroup);
    if (handle.isError()) {
      infos.clear();
      return Failure(
          "Failed to recover the net_cls handle for orphan container " +
          stringify(containerId) + ": " + handle.error());
    }

    // Known orphan cgroups will be destroyed by the containerizer
    // using the normal cleanup path. See MESOS-2367 for details.
    if (orphans.contains(containerId)) {
      if (handle.isSome()) {
        infos.emplace(containerId, Info(cgroup, handle.get()));
      } else {
        infos.emplace(containerId, Info(cgroup));
      }

      continue;
    }

    LOG(INFO) << "Removing unknown orphaned cgroup '" << cgroup << "'";

    // We don't wait on the destroy as we don't want to block recovery.
    // TODO(jieyu): Release the handle after destroy has been done.
    cgroups::destroy(hierarchy, cgroup, cgroups::DESTROY_TIMEOUT);
  }

  return Nothing();
}


// TODO(asridharan): Currently we haven't decided on the entity who
// will allocate the net_cls handles, or the interfaces through which
// the net_cls handles will be exposed to network isolators and
// frameworks. Once the management entity is decided we might need to
// revisit this implementation.
Future<Nothing> CgroupsNetClsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ContainerStatus> CgroupsNetClsIsolatorProcess::status(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Info& info = infos.at(containerId);

  ContainerStatus status;

  if (info.handle.isSome()) {
    VLOG(1) << "Updating container status with net_cls classid: "
            << info.handle.get();

    CgroupInfo* cgroupInfo = status.mutable_cgroup_info();
    CgroupInfo::NetCls* netCls = cgroupInfo->mutable_net_cls();

    netCls->set_classid(info.handle->get());
  }

  return status;
}


Future<Option<ContainerLaunchInfo>> CgroupsNetClsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // Use this info to create the cgroup, but do not insert it into
  // infos till the cgroup has been created successfully.
  const string cgroup = path::join(flags.cgroups_root, containerId.value());

  // Create a cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchy, cgroup);
  if (exists.isError()) {
    return Failure(
        "Failed to check if the cgroup already exists: " + exists.error());
  } else if (exists.get()) {
    return Failure("The cgroup already exists");
  }

  Try<Nothing> create = cgroups::create(hierarchy, cgroup);
  if (create.isError()) {
    return Failure("Failed to create the cgroup: " + create.error());
  }

  // 'chown' the cgroup so the executor can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  if (containerConfig.has_user()) {
    Try<Nothing> chown = os::chown(
        containerConfig.user(),
        path::join(hierarchy, cgroup),
        false);

    if (chown.isError()) {
      return Failure(
          "Failed to change ownership of cgroup hierarchy: " + chown.error());
    }
  }

  if (handleManager.isSome()) {
    Try<NetClsHandle> handle = handleManager->alloc();
    if (handle.isError()) {
      return Failure (
          "Failed to allocate a net_cls handle: " +
          handle.error());
    }

    LOG(INFO) << "Allocated handle: " << handle.get()
              << " to container " << containerId;

    infos.emplace(containerId, Info(cgroup, handle.get()));
  } else {
    infos.emplace(containerId, Info(cgroup));
  }

  return update(containerId, containerConfig.executor_info().resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<Nothing> CgroupsNetClsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Info& info = infos.at(containerId);

  Try<Nothing> assign = cgroups::assign(hierarchy, info.cgroup, pid);
  if (assign.isError()) {
    return Failure(
        "Failed to assign container " + stringify(containerId) +
        " to its own cgroup '" + path::join(hierarchy, info.cgroup) +
        "': " + assign.error());
  }

  // If info.handle is not specified, the assumption is that the
  // operator is responsible for assigning the net_cls handles.
  if (info.handle.isSome()) {
    Try<Nothing> write = cgroups::net_cls::classid(
        hierarchy,
        info.cgroup,
        info.handle->get());

    if (write.isError()) {
      return Failure(
          "Failed to assign a net_cls handle to the cgroup: " +
          write.error());
    }
  }

  return Nothing();
}


// The net_cls handles are labels and hence there are no limitations
// associated with them. This function would therefore always return
// a pending future since the limitation is never reached.
Future<ContainerLimitation> CgroupsNetClsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return Future<ContainerLimitation>();
}


Future<Nothing> CgroupsNetClsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;
    return Nothing();
  }

  const Info& info = infos.at(containerId);

  return cgroups::destroy(hierarchy, info.cgroup, cgroups::DESTROY_TIMEOUT)
    .then(defer(PID<CgroupsNetClsIsolatorProcess>(this),
                &CgroupsNetClsIsolatorProcess::_cleanup,
                containerId));
}


Future<Nothing> CgroupsNetClsIsolatorProcess::_cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Nothing();
  }

  const Info& info = infos.at(containerId);

  if (info.handle.isSome() && handleManager.isSome()) {
    Try<Nothing> free = handleManager->free(info.handle.get());
    if (free.isError()) {
      return Failure("Could not free the net_cls handle: " + free.error());
    }
  }

  infos.erase(containerId);

  return Nothing();
}


Result<NetClsHandle> CgroupsNetClsIsolatorProcess::recoverHandle(
    const std::string& hierarchy,
    const std::string& cgroup)
{
  Try<uint32_t> classid = cgroups::net_cls::classid(hierarchy, cgroup);
  if (classid.isError()) {
    return Error("Failed to read 'net_cls.classid': " + classid.error());
  }

  if (classid.get() == 0) {
    return None();
  }

  NetClsHandle handle(classid.get());

  // Mark the handle as used in handle manager.
  if (handleManager.isSome()) {
    Try<Nothing> reserve = handleManager->reserve(handle);
    if (reserve.isError()) {
      return Error("Failed to reserve the handle: " + reserve.error());
    }
  }

  return handle;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
