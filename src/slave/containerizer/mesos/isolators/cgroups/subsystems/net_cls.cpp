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

#include <vector>

#include <process/id.hpp>

#include <stout/unreachable.hpp>

#include "linux/cgroups.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.hpp"

using process::Failure;
using process::Future;
using process::Owned;

using std::ostream;
using std::string;
using std::stringstream;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

static string hexify(uint32_t handle)
{
  stringstream stream;
  stream << std::hex << handle;
  return "0x" + stream.str();
};


ostream& operator<<(ostream& stream, const NetClsHandle& obj)
{
  return stream << hexify(obj.get());
}


NetClsHandleManager::NetClsHandleManager(
    const IntervalSet<uint32_t>& _primaries,
    const IntervalSet<uint32_t>& _secondaries)
    : primaries(_primaries),
      secondaries(_secondaries)
{
  if (secondaries.empty()) {
    // Default range [1,0xffff].
    secondaries +=
        (Bound<uint32_t>::closed(1),
         Bound<uint32_t>::closed(0xffff));
  }
};


// For each primary handle, we maintain a bitmap to keep track of
// allocated and free secondary handles. To find a free secondary
// handle we scan the bitmap from the first element till we find a
// free handle.
//
// TODO(asridharan): Currently the bitmap search is naive, since the
// assumption is that the number of containers running on an agent
// will be O(100). If we start facing any performance issues, we might
// want to revisit this logic and make the search for a free secondary
// handle more efficient. One idea for making it more efficient would
// be to store the last allocated handle and start the search at this
// position and performing a circular search on the bitmap.
Try<NetClsHandle> NetClsHandleManager::alloc(
    const Option<uint16_t>& _primary)
{
  uint16_t primary;
  if (_primary.isNone()) {
    // Currently, the interval set `primaries` is assumed to be a
    // singleton. The singleton is used as the primary handle for all
    // net_cls operations in this isolator. In the future, the
    // `cgroups/net_cls` isolator will take in ranges instead of a
    // singleton value. At that point we might not need a default
    // primary handle.
    primary = (*primaries.begin()).lower();
  } else {
    primary = _primary.get();
  }

  if (!primaries.contains(primary)) {
    return Error(
        "Primary handle " + hexify(primary) +
        " not present in primary handle range");
  }

  if (!used.contains(primary)) {
    used[primary].set(); // Set all handles.

    foreach (const Interval<uint32_t>& handles, secondaries) {
      for (size_t secondary = handles.lower();
           secondary < handles.upper(); secondary++) {
        used[primary].reset(secondary);
      }
    }
  } else if (used[primary].all()) {
    return Error(
        "No free handles remaining for primary handle " +
        hexify(primary));
  }

  // At least one secondary handle is free for this primary handle.
  for (size_t secondary = 1; secondary < used[primary].size(); secondary++) {
    if (!used[primary].test(secondary)) {
      used[primary].set(secondary);

      return NetClsHandle(primary, secondary);
    }
  }

  UNREACHABLE();
}


Try<Nothing> NetClsHandleManager::reserve(const NetClsHandle& handle)
{
  if (!primaries.contains(handle.primary)) {
    return Error(
        "Primary handle " + hexify(handle.primary) +
        " not present in primary handle range");
  }

  if (!secondaries.contains(handle.secondary)) {
    return Error(
        "Secondary handle " + hexify(handle.secondary) +
        " not present in secondary handle range ");
  }

  if (!used.contains(handle.primary)) {
    used[handle.primary].set(); // Set all handles.

    foreach (const Interval<uint32_t>& handles, secondaries) {
      for (size_t secondary = handles.lower();
           secondary < handles.upper(); secondary++) {
        used[handle.primary].reset(secondary);
      }
    }
  }

  if (used[handle.primary].test(handle.secondary)) {
    return Error(
        "The secondary handle " + hexify(handle.secondary) +
        ", for the primary handle " + hexify(handle.primary) +
        " has already been allocated");
  }

  used[handle.primary].set(handle.secondary);

  return Nothing();
}


Try<Nothing> NetClsHandleManager::free(const NetClsHandle& handle)
{
  if (!primaries.contains(handle.primary)) {
    return Error(
        "Primary handle " + hexify(handle.primary) +
        " not present in primary handle range");
  }

  if (!secondaries.contains(handle.secondary)) {
    return Error(
        "Secondary handle " + hexify(handle.secondary) +
        " not present in secondary handle range ");
  }

  if (!used.contains(handle.primary)) {
    return Error(
        "No secondary handles have been allocated from this primary handle " +
        hexify(handle.primary));
  }

  if (!used[handle.primary].test(handle.secondary)) {
    return Error(
        "Secondary handle " + hexify(handle.secondary) +
        " is not allocated for primary handle " +
        hexify(handle.primary));
  }

  used[handle.primary].reset(handle.secondary);

  return Nothing();
}


Try<bool> NetClsHandleManager::isUsed(const NetClsHandle& handle)
{
  if (!primaries.contains(handle.primary)) {
    return Error(
        "Primary handle: " + hexify(handle.primary) +
        " is not within the primary's range");
  }

  if (!secondaries.contains(handle.secondary)) {
    return Error(
        "Secondary handle " + hexify(handle.secondary) +
        " not present in secondary handle range");
  }

  if (!used.contains(handle.primary)) {
    return false;
  }

  return used[handle.primary].test(handle.secondary);
}


Try<Owned<SubsystemProcess>> NetClsSubsystemProcess::create(
    const Flags& flags,
    const string& hierarchy)
{
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
            "Failed to parse the range of secondary handles '" +
            flags.cgroups_net_cls_secondary_handles.get() +
            "' set in flag --cgroups_net_cls_secondary_handles");
      }

      Try<uint16_t> lower = numify<uint16_t>(range[0]);
      if (lower.isError()) {
        return Error(
            "Failed to parse the lower bound of range of secondary handles '" +
            flags.cgroups_net_cls_secondary_handles.get() +
            "' set in flag --cgroups_net_cls_secondary_handles");
      }

      if (lower.get() == 0) {
        return Error("The secondary handle has to be a non-zero value.");
      }

      Try<uint16_t> upper = numify<uint16_t>(range[1]);
      if (upper.isError()) {
        return Error(
            "Failed to parse the upper bound of range of secondary handles '" +
            flags.cgroups_net_cls_secondary_handles.get() +
            "' set in flag --cgroups_net_cls_secondary_handles");
      }

      secondaries +=
        (Bound<uint32_t>::closed(lower.get()),
         Bound<uint32_t>::closed(upper.get()));

      if (secondaries.empty()) {
        return Error(
            "Secondary handle range specified '" +
            flags.cgroups_net_cls_secondary_handles.get() +
            "', in flag --cgroups_net_cls_secondary_handles, is an empty set");
      }
    }
  }

  return Owned<SubsystemProcess>(
      new NetClsSubsystemProcess(flags, hierarchy, primaries, secondaries));
}


NetClsSubsystemProcess::NetClsSubsystemProcess(
    const Flags& _flags,
    const string& _hierarchy,
    const IntervalSet<uint32_t>& primaries,
    const IntervalSet<uint32_t>& secondaries)
  : ProcessBase(process::ID::generate("cgroups-net-cls-subsystem")),
    SubsystemProcess(_flags, _hierarchy)
{
  if (!primaries.empty()) {
    handleManager = NetClsHandleManager(primaries, secondaries);
  }
}


Future<Nothing> NetClsSubsystemProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (infos.contains(containerId)) {
    return Failure("The subsystem '" + name() + "' has already been recovered");
  }

  // Read the net_cls handle.
  Result<NetClsHandle> handle = recoverHandle(hierarchy, cgroup);

  if (handle.isError()) {
    return Failure(
        "Failed to recover the net_cls handle: " + handle.error());
  }

  if (handle.isSome()) {
    infos.put(containerId, Owned<Info>(new Info(handle.get())));
  } else {
    infos.put(containerId, Owned<Info>(new Info));
  }

  return Nothing();
}


Future<Nothing> NetClsSubsystemProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("The subsystem '" + name() + "' has already been prepared");
  }

  if (handleManager.isSome()) {
    Try<NetClsHandle> handle = handleManager->alloc();
    if (handle.isError()) {
      return Failure(
          "Failed to allocate a net_cls handle: " + handle.error());
    }

    LOG(INFO) << "Allocated a net_cls handle: " << handle.get()
              << " to container " << containerId;

    infos.put(containerId, Owned<Info>(new Info(handle.get())));
  } else {
    infos.put(containerId, Owned<Info>(new Info));
  }

  return Nothing();
}


Future<Nothing> NetClsSubsystemProcess::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to isolate subsystem '" + name() + "'"
        ": Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  // If handle is not specified, the assumption is that the operator is
  // responsible for assigning the net_cls handles.
  if (info->handle.isSome()) {
    Try<Nothing> write = cgroups::net_cls::classid(
        hierarchy,
        cgroup,
        info->handle->get());

    if (write.isError()) {
      return Failure(
          "Failed to assign a net_cls handle to the cgroup"
          ": " + write.error());
    }
  }

  return Nothing();
}


Future<ContainerStatus> NetClsSubsystemProcess::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to get the status of subsystem '" + name() + "'"
        ": Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  ContainerStatus result;

  if (info->handle.isSome()) {
    VLOG(1) << "Updating container status with net_cls classid: "
            << info->handle.get();

    CgroupInfo* cgroupInfo = result.mutable_cgroup_info();
    CgroupInfo::NetCls* netCls = cgroupInfo->mutable_net_cls();

    netCls->set_classid(info->handle->get());
  }

  return result;
}


Future<Nothing> NetClsSubsystemProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup subsystem '" << name() << "' "
            << "request for unknown container " << containerId;

    return Nothing();
  }

  const Owned<Info>& info = infos[containerId];

  if (info->handle.isSome() && handleManager.isSome()) {
    Try<Nothing> free = handleManager->free(info->handle.get());
    if (free.isError()) {
      return Failure("Could not free the net_cls handle: " + free.error());
    }
  }

  infos.erase(containerId);

  return Nothing();
}


Result<NetClsHandle> NetClsSubsystemProcess::recoverHandle(
    const string& hierarchy,
    const string& cgroup)
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
