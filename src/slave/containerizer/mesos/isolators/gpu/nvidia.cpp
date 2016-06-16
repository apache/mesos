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

#include "slave/containerizer/mesos/isolators/gpu/nvidia.hpp"
#include "slave/containerizer/mesos/isolators/gpu/nvml.hpp"

using cgroups::devices::Entry;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

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

// TODO(klueska): Expand this when we support other GPU types.
static constexpr unsigned int NVIDIA_MAJOR_DEVICE = 195;


NvidiaGpuIsolatorProcess::NvidiaGpuIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy,
    const list<Gpu>& gpus,
    const cgroups::devices::Entry& uvmDeviceEntry,
    const cgroups::devices::Entry& ctlDeviceEntry)
  : flags(_flags),
    hierarchy(_hierarchy),
    available(gpus),
    NVIDIA_CTL_DEVICE_ENTRY(ctlDeviceEntry),
    NVIDIA_UVM_DEVICE_ENTRY(uvmDeviceEntry) {}


Try<Isolator*> NvidiaGpuIsolatorProcess::create(const Flags& flags)
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
                 " order to use the gpu/devices isolator");
  }

  if (devicesIsolator > gpuIsolator) {
    return Error("'cgroups/devices' must precede 'gpu/nvidia'"
                 " in the --isolation flag");
  }

  // Initialize NVML.
  Try<Nothing> initialize = nvml::initialize();
  if (initialize.isError()) {
    return Error("Failed to initialize nvml: " + initialize.error());
  }

  // Grab the full list of resources computed for this containerizer
  // and filter it down to just the GPU resources.
  //
  // TODO(klueska): Calling into the containerizer helper here is
  // a hack. Consider passing the resources directly into `create`.
  Try<Resources> resources = Containerizer::resources(flags);
  if (resources.isError()) {
    return Error(resources.error());
  }

  // Figure out the list of GPUs to make available by their index.
  vector<unsigned int> indices;

  if (flags.nvidia_gpu_devices.isSome()) {
    indices = flags.nvidia_gpu_devices.get();
  } else if (resources->gpus().isSome()) {
    for (unsigned int i = 0; i < resources->gpus().get(); ++i) {
      indices.push_back(i);
    }
  }

  // Build the list of available GPUs using the ids computed above.
  list<Gpu> gpus;

  foreach (unsigned int index, indices) {
    Try<nvmlDevice_t> handle = nvml::deviceGetHandleByIndex(index);
    if (handle.isError()) {
      return Error("Failed to obtain Nvidia device handle for"
                   " index " + stringify(index) + ": " + handle.error());
    }

    Try<unsigned int> minor = nvml::deviceGetMinorNumber(handle.get());
    if (minor.isError()) {
      return Error("Failed to obtain Nvidia device minor number: " +
                   minor.error());
    }

    Gpu gpu;
    gpu.handle = handle.get();
    gpu.major = NVIDIA_MAJOR_DEVICE;
    gpu.minor = minor.get();

    gpus.push_back(gpu);
  }

  // Retrieve the cgroups devices hierarchy.
  Result<string> hierarchy = cgroups::hierarchy("devices");

  if (hierarchy.isError()) {
    return Error(
        "Error retrieving the 'devices' subsystem hierarchy: " +
        hierarchy.error());
  }

  // Create the device entries for
  // `/dev/nvidiactl` and `/dev/nvidia-uvm`.
  Try<dev_t> device = os::stat::rdev("/dev/nvidiactl");
  if (device.isError()) {
    return Error("Failed to obtain device ID for '/dev/nvidiactl': " +
                 device.error());
  }

  cgroups::devices::Entry ctlDeviceEntry;
  ctlDeviceEntry.selector.type = Entry::Selector::Type::CHARACTER;
  ctlDeviceEntry.selector.major = major(device.get());
  ctlDeviceEntry.selector.minor = minor(device.get());
  ctlDeviceEntry.access.read = true;
  ctlDeviceEntry.access.write = true;
  ctlDeviceEntry.access.mknod = true;

  device = os::stat::rdev("/dev/nvidia-uvm");
  if (device.isError()) {
    return Error("Failed to obtain device ID for '/dev/nvidia-uvm': " +
                 device.error());
  }

  cgroups::devices::Entry uvmDeviceEntry;
  uvmDeviceEntry.selector.type = Entry::Selector::Type::CHARACTER;
  uvmDeviceEntry.selector.major = major(device.get());
  uvmDeviceEntry.selector.minor = minor(device.get());
  uvmDeviceEntry.access.read = true;
  uvmDeviceEntry.access.write = true;
  uvmDeviceEntry.access.mknod = true;

  process::Owned<MesosIsolatorProcess> process(
      new NvidiaGpuIsolatorProcess(
          flags,
          hierarchy.get(),
          gpus,
          ctlDeviceEntry,
          uvmDeviceEntry));

  return new MesosIsolator(process);
}


Future<Nothing> NvidiaGpuIsolatorProcess::recover(
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

    foreach (const cgroups::devices::Entry& entry, entries.get()) {
      for (auto gpu = available.begin(); gpu != available.end(); ++gpu) {
        if (entry.selector.major == gpu->major &&
            entry.selector.minor == gpu->minor) {
          infos[containerId]->allocated.push_back(*gpu);
          available.erase(gpu);
          break;
        }
      }
    }
  }

  return Nothing();
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

  // Grant access to /dev/nvidiactl and /dev/nvida-uvm.
  //
  // This allows standard NVIDIA tools like `nvidia-smi` to be
  // used within the container even if no GPUs are allocated.
  // Without these devices, these tools fail abnormally.
  map<string, const cgroups::devices::Entry> entries = {
    { "/dev/nvidiactl", NVIDIA_CTL_DEVICE_ENTRY },
    { "/dev/nvidia-uvm", NVIDIA_UVM_DEVICE_ENTRY },
  };

  foreachkey (const string& device, entries) {
    Try<Nothing> allow = cgroups::devices::allow(
        hierarchy, infos[containerId]->cgroup, entries[device]);

    if (allow.isError()) {
      return Failure("Failed to grant cgroups access to"
                     " '" + device + "': " + allow.error());
    }
  }

  return update(containerId, containerConfig.executor_info().resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
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

    if (additional > available.size()) {
      return Failure("Not enough GPUs available to reserve"
                     " " + stringify(additional) + " additional GPUs");
    }

    for (size_t i = 0; i < additional; i++) {
      const Gpu& gpu = available.front();

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

      info->allocated.push_back(gpu);
      available.pop_front();
    }
  } else if (requested < info->allocated.size()) {
    size_t fewer = info->allocated.size() - requested;

    for (size_t i = 0; i < fewer; i++) {
      const Gpu& gpu = info->allocated.front();

      cgroups::devices::Entry entry;
      entry.selector.type = Entry::Selector::Type::CHARACTER;
      entry.selector.major = gpu.major;
      entry.selector.minor = gpu.minor;
      entry.access.read = true;
      entry.access.write = true;
      entry.access.mknod = true;

      Try<Nothing> deny = cgroups::devices::deny(
          hierarchy, info->cgroup, entry);

      if (deny.isError()) {
        return Failure("Failed to deny cgroups access to GPU device"
                       " '" + stringify(entry) + "': " + deny.error());
      }

      info->allocated.pop_front();
      available.push_back(gpu);
    }
  }

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

  Info* info = CHECK_NOTNULL(infos[containerId]);

  // Make any remaining GPUs available.
  available.splice(available.end(), info->allocated);

  delete info;
  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
