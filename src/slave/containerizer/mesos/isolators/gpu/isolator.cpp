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

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/gpu/allocator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/isolator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/nvml.hpp"

using cgroups::devices::Entry;

using docker::spec::v1::ImageManifest;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::defer;
using process::Failure;
using process::Future;
using process::PID;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

NvidiaGpuIsolatorProcess::NvidiaGpuIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy,
    const NvidiaGpuAllocator& _allocator,
    const NvidiaVolume& _volume,
    const map<Path, cgroups::devices::Entry>& _controlDeviceEntries)
  : ProcessBase(process::ID::generate("mesos-nvidia-gpu-isolator")),
    flags(_flags),
    hierarchy(_hierarchy),
    allocator(_allocator),
    volume(_volume),
    controlDeviceEntries(_controlDeviceEntries) {}


Try<Isolator*> NvidiaGpuIsolatorProcess::create(
    const Flags& flags,
    const NvidiaComponents& components)
{
  // Make sure the 'cgroups/devices' isolator is present and
  // precedes the GPU isolator.
  vector<string> tokens = strings::tokenize(flags.isolation, ",");

  auto gpuIsolator =
    std::find(tokens.begin(), tokens.end(), "gpu/nvidia");
  auto devicesIsolator =
    std::find(tokens.begin(), tokens.end(), "cgroups/devices");

  CHECK(gpuIsolator != tokens.end());

  if (devicesIsolator == tokens.end()) {
    return Error("The 'cgroups/devices' isolator must be enabled in"
                 " order to use the 'gpu/nvidia' isolator");
  }

  if (devicesIsolator > gpuIsolator) {
    return Error("'cgroups/devices' must precede 'gpu/nvidia'"
                 " in the --isolation flag");
  }

  // Retrieve the cgroups devices hierarchy.
  Result<string> hierarchy = cgroups::hierarchy("devices");

  if (hierarchy.isError()) {
    return Error(
        "Error retrieving the 'devices' subsystem hierarchy: " +
        hierarchy.error());
  }

  // Create device entries for `/dev/nvidiactl` and
  // `/dev/nvidia-uvm`. Optionally create a device entry for
  // `/dev/nvidia-uvm-tools` if it exists.
  map<Path, cgroups::devices::Entry> deviceEntries;

  Try<dev_t> device = os::stat::rdev("/dev/nvidiactl");
  if (device.isError()) {
    return Error("Failed to obtain device ID for '/dev/nvidiactl': " +
                 device.error());
  }

  cgroups::devices::Entry entry;
  entry.selector.type = Entry::Selector::Type::CHARACTER;
  entry.selector.major = major(device.get());
  entry.selector.minor = minor(device.get());
  entry.access.read = true;
  entry.access.write = true;
  entry.access.mknod = true;

  deviceEntries[Path("/dev/nvidiactl")] = entry;

  device = os::stat::rdev("/dev/nvidia-uvm");
  if (device.isError()) {
    return Error("Failed to obtain device ID for '/dev/nvidia-uvm': " +
                 device.error());
  }

  entry.selector.type = Entry::Selector::Type::CHARACTER;
  entry.selector.major = major(device.get());
  entry.selector.minor = minor(device.get());
  entry.access.read = true;
  entry.access.write = true;
  entry.access.mknod = true;

  deviceEntries[Path("/dev/nvidia-uvm")] = entry;

  device = os::stat::rdev("/dev/nvidia-uvm-tools");
  if (device.isSome()) {
    entry.selector.type = Entry::Selector::Type::CHARACTER;
    entry.selector.major = major(device.get());
    entry.selector.minor = minor(device.get());
    entry.access.read = true;
    entry.access.write = true;
    entry.access.mknod = true;

    deviceEntries[Path("/dev/nvidia-uvm-tools")] = entry;
  }

  process::Owned<MesosIsolatorProcess> process(
      new NvidiaGpuIsolatorProcess(
          flags,
          hierarchy.get(),
          components.allocator,
          components.volume,
          deviceEntries));

  return new MesosIsolator(process);
}


Future<Nothing> NvidiaGpuIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  list<Future<Nothing>> futures;

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
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);

    // Determine which GPUs are allocated to this container.
    Try<vector<cgroups::devices::Entry>> entries =
      cgroups::devices::list(hierarchy, cgroup);

    if (entries.isError()) {
      return Failure("Failed to obtain devices list for cgroup"
                     " '" + cgroup + "': " + entries.error());
    }

    const set<Gpu>& available = allocator.total();

    set<Gpu> containerGpus;
    foreach (const cgroups::devices::Entry& entry, entries.get()) {
      foreach (const Gpu& gpu, available) {
        if (entry.selector.major == gpu.major &&
            entry.selector.minor == gpu.minor) {
          containerGpus.insert(gpu);
          break;
        }
      }
    }

    futures.push_back(allocator.allocate(containerGpus)
      .then(defer(self(), [=]() -> Future<Nothing> {
        infos[containerId]->allocated = containerGpus;
        return Nothing();
      })));
  }

  return collect(futures).then([]() { return Nothing(); });
}


Future<Option<ContainerLaunchInfo>> NvidiaGpuIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos[containerId] = new Info(
      containerId, path::join(flags.cgroups_root, containerId.value()));

  // Grant access to all `controlDeviceEntries`.
  //
  // This allows standard NVIDIA tools like `nvidia-smi` to be
  // used within the container even if no GPUs are allocated.
  // Without these devices, these tools fail abnormally.
  foreachkey (const Path& devicePath, controlDeviceEntries) {
    Try<Nothing> allow = cgroups::devices::allow(
        hierarchy,
        infos[containerId]->cgroup,
        controlDeviceEntries.at(devicePath));

    if (allow.isError()) {
      return Failure("Failed to grant cgroups access to"
                     " '" + stringify(devicePath) + "': " + allow.error());
    }
  }

  return update(containerId, containerConfig.executor_info().resources())
    .then(defer(PID<NvidiaGpuIsolatorProcess>(this),
                &NvidiaGpuIsolatorProcess::_prepare,
                containerConfig));
}


// If our `ContainerConfig` specifies a different `rootfs` than the
// host file system, then we need to prepare a script to inject our
// `NvidiaVolume` into the container (if required).
Future<Option<ContainerLaunchInfo>> NvidiaGpuIsolatorProcess::_prepare(
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (!containerConfig.has_rootfs()) {
     return None();
  }

  // We only support docker containers at the moment.
  if (!containerConfig.has_docker()) {
    // TODO(klueska): Once ContainerConfig has
    // a type, include that in the error message.
    return Failure("Nvidia GPU isolator does not support non-Docker images");
  }

  ContainerLaunchInfo launchInfo;
  launchInfo.set_namespaces(CLONE_NEWNS);

  // Inject the Nvidia volume into the container.
  //
  // TODO(klueska): Inject the Nvidia devices here as well once we
  // have a way to pass them to `fs:enter()` instead of hardcoding
  // them in `fs::createStandardDevices()`.
  if (!containerConfig.docker().has_manifest()) {
     return Failure("The 'ContainerConfig' for docker is missing a manifest");
  }

  ImageManifest manifest = containerConfig.docker().manifest();

  if (volume.shouldInject(manifest)) {
    const string target = path::join(
        containerConfig.rootfs(),
        volume.CONTAINER_PATH());

    Try<Nothing> mkdir = os::mkdir(target);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create the container directory at"
          " '" + target + "': " + mkdir.error());
    }

    launchInfo.add_pre_exec_commands()->set_value(
      "mount --no-mtab --rbind --read-only " +
      volume.HOST_PATH() + " " + target);
  }

  return launchInfo;
}


Future<Nothing> NvidiaGpuIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  Option<double> gpus = resources.gpus();

  // Make sure that the `gpus` resource is not fractional.
  // We rely on scalar resources only having 3 digits of precision.
  if (static_cast<long long>(gpus.getOrElse(0.0) * 1000.0) % 1000 != 0) {
    return Failure("The 'gpus' resource must be an unsigned integer");
  }

  size_t requested = static_cast<size_t>(resources.gpus().getOrElse(0.0));

  // Update the GPU allocation to reflect the new total.
  if (requested > info->allocated.size()) {
    size_t additional = requested - info->allocated.size();

    return allocator.allocate(additional)
      .then(defer(PID<NvidiaGpuIsolatorProcess>(this),
                  &NvidiaGpuIsolatorProcess::_update,
                  containerId,
                  lambda::_1));
  } else if (requested < info->allocated.size()) {
    size_t fewer = info->allocated.size() - requested;

    set<Gpu> deallocated;

    for (size_t i = 0; i < fewer; i++) {
      const auto gpu = info->allocated.begin();

      cgroups::devices::Entry entry;
      entry.selector.type = Entry::Selector::Type::CHARACTER;
      entry.selector.major = gpu->major;
      entry.selector.minor = gpu->minor;
      entry.access.read = true;
      entry.access.write = true;
      entry.access.mknod = true;

      Try<Nothing> deny = cgroups::devices::deny(
          hierarchy, info->cgroup, entry);

      if (deny.isError()) {
        return Failure("Failed to deny cgroups access to GPU device"
                       " '" + stringify(entry) + "': " + deny.error());
      }

      deallocated.insert(*gpu);
      info->allocated.erase(gpu);
    }

    return allocator.deallocate(deallocated);
  }

  return Nothing();
}


Future<Nothing> NvidiaGpuIsolatorProcess::_update(
    const ContainerID& containerId,
    const set<Gpu>& allocation)
{
  if (!infos.contains(containerId)) {
    return Failure("Failed to complete GPU allocation: unknown container");
  }

  Info* info = CHECK_NOTNULL(infos.at(containerId));

  foreach (const Gpu& gpu, allocation) {
    cgroups::devices::Entry entry;
    entry.selector.type = Entry::Selector::Type::CHARACTER;
    entry.selector.major = gpu.major;
    entry.selector.minor = gpu.minor;
    entry.access.read = true;
    entry.access.write = true;
    entry.access.mknod = true;

    Try<Nothing> allow = cgroups::devices::allow(
        hierarchy, info->cgroup, entry);

    if (allow.isError()) {
      return Failure("Failed to grant cgroups access to GPU device"
                     " '" + stringify(entry) + "': " + allow.error());
    }
  }

  info->allocated = allocation;

  return Nothing();
}


Future<ResourceStatistics> NvidiaGpuIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  // TODO(rtodd): Obtain usage information from NVML.

  ResourceStatistics result;
  return result;
}


Future<Nothing> NvidiaGpuIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos.at(containerId));

  // Make any remaining GPUs available.
  return allocator.deallocate(info->allocated)
    .then(defer(self(), [=]() -> Future<Nothing> {
      CHECK(infos.contains(containerId));
      delete infos.at(containerId);
      infos.erase(containerId);

      return Nothing();
    }));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
