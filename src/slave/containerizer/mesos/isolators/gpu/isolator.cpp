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

#include <sys/mount.h>

// This header include must be enclosed in an `extern "C"` block to
// workaround a bug in glibc <= 2.12 (see MESOS-7378).
//
// TODO(neilc): Remove this when we no longer support glibc <= 2.12.
extern "C" {
#include <sys/sysmacros.h>
}

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

#include "common/protobuf_utils.hpp"

#include "linux/cgroups.hpp"
#include "linux/fs.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

#include "slave/containerizer/mesos/isolators/gpu/allocator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/isolator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/nvml.hpp"

#include "slave/containerizer/mesos/paths.hpp"

using cgroups::devices::Entry;

using docker::spec::v1::ImageManifest;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerMountInfo;
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
  // Make sure both the 'cgroups/devices' (or 'cgroups/all')
  // and the 'filesystem/linux' isolators are present.
  vector<string> tokens = strings::tokenize(flags.isolation, ",");

  auto gpuIsolator =
    std::find(tokens.begin(), tokens.end(), "gpu/nvidia");

  auto devicesIsolator =
    std::find(tokens.begin(), tokens.end(), "cgroups/devices");

  auto cgroupsAllIsolator =
    std::find(tokens.begin(), tokens.end(), "cgroups/all");

  auto filesystemIsolator =
    std::find(tokens.begin(), tokens.end(), "filesystem/linux");

  CHECK(gpuIsolator != tokens.end());

  if (cgroupsAllIsolator != tokens.end()) {
    // The reason that we need to check if `devices` cgroups subsystem is
    // enabled is, when `cgroups/all` is specified in the `--isolation` agent
    // flag, cgroups isolator will only load the enabled subsystems. So if
    // `cgroups/all` is specified but `devices` is not enabled, cgroups isolator
    // will not load `devices` subsystem in which case we should error out.
    Try<bool> result = cgroups::enabled("devices");
    if (result.isError()) {
      return Error(
          "Failed to check if the `devices` cgroups subsystem"
          " is enabled by kernel: " + result.error());
    } else if (!result.get()) {
      return Error(
          "The `devices` cgroups subsystem is not enabled by the kernel");
    }
  } else if (devicesIsolator == tokens.end()) {
    return Error(
        "The 'cgroups/devices' or 'cgroups/all' isolator must be"
        " enabled in order to use the 'gpu/nvidia' isolator");
  }

  if (filesystemIsolator == tokens.end()) {
    return Error("The 'filesystem/linux' isolator must be enabled in"
                 " order to use the 'gpu/nvidia' isolator");
  }

  // Retrieve the cgroups devices hierarchy.
  Result<string> hierarchy = cgroups::hierarchy(CGROUP_SUBSYSTEM_DEVICES_NAME);

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

  // The `nvidia-uvm` module is not typically loaded by default on
  // systems that have Nvidia GPU drivers installed. Instead,
  // applications that require this module use `nvidia-modprobe` to
  // load it dynamically on first use. This program both loads the
  // `nvidia-uvm` kernel module and creates the corresponding
  // `/dev/nvidia-uvm` device that it controls.
  //
  // We call `nvidia-modprobe` here to ensure that `/dev/nvidia-uvm`
  // is properly created so we can inject it into any containers that
  // may require it.
  if (!os::exists("/dev/nvidia-uvm")) {
    Try<string> modprobe = os::shell("nvidia-modprobe -u -c 0");
    if (modprobe.isError()) {
      return Error("Failed to load '/dev/nvidia-uvm': " + modprobe.error());
    }
  }

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


bool NvidiaGpuIsolatorProcess::supportsNesting()
{
  return true;
}


bool NvidiaGpuIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Nothing> NvidiaGpuIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  vector<Future<Nothing>> futures;

  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();

    // If we are a nested container, we skip the recover because our
    // root ancestor will recover the GPU state from the cgroup for us.
    if (containerId.has_parent()) {
      continue;
    }

    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    if (!cgroups::exists(hierarchy, cgroup)) {
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      LOG(WARNING) << "Couldn't find the cgroup '" << cgroup << "' "
                   << "in hierarchy '" << hierarchy << "' "
                   << "for container " << containerId;
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
  if (containerId.has_parent()) {
    // If we are a nested container in the `DEBUG` class, then we
    // don't need to do anything special to prepare ourselves for GPU
    // support. All Nvidia volumes will be inherited from our parent.
    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return None();
    }

    // If we are a nested container in a different class, we don't
    // need to maintain an `Info()` struct about the container (since
    // we don't directly allocate any GPUs to it), but we do need to
    // mount the necessary Nvidia libraries into the container (since
    // we live in a different mount namespace than our parent). We
    // directly call `_prepare()` to do this for us.
    return _prepare(containerId, containerConfig);
  }

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

  return update(containerId, containerConfig.resources())
    .then(defer(PID<NvidiaGpuIsolatorProcess>(this),
                &NvidiaGpuIsolatorProcess::_prepare,
                containerId,
                containerConfig));
}


// If our `ContainerConfig` specifies a different `rootfs` than the
// host file system, then we need to prepare a script to inject our
// `NvidiaVolume` into the container (if required).
Future<Option<ContainerLaunchInfo>> NvidiaGpuIsolatorProcess::_prepare(
    const ContainerID& containerId,
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

  // Inject the Nvidia volume into the container.
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

    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        volume.HOST_PATH(), target, MS_RDONLY | MS_BIND | MS_REC);

    // TODO(chhsiao): As a workaround, we append `NvidiaVolume` paths into the
    // `PATH` and `LD_LIBRARY_PATH` environment variables so the binaries and
    // libraries can be found. However these variables might be overridden by
    // users, and `LD_LIBRARY_PATH` might get cleared across exec calls. Instead
    // of injecting `NvidiaVolume`, we could leverage libnvidia-container in the
    // future. See MESOS-9595.
    if (containerConfig.has_task_info()) {
      // Command executor.
      *launchInfo.mutable_task_environment() = volume.ENV(manifest);
    } else {
      // Default executor, custom executor, or nested container.
      *launchInfo.mutable_environment() = volume.ENV(manifest);
    }
  }

  const string devicesDir = containerizer::paths::getContainerDevicesPath(
      flags.runtime_dir, containerId);

  // The `filesystem/linux` isolator is responsible for creating the
  // devices directory and ordered to run before we do. Here, we can
  // just assert that the devices directory is still present.
  if (!os::exists(devicesDir)) {
    return Failure("Missing container devices directory '" + devicesDir + "'");
  }

  // Glob all Nvidia GPU devices on the system and add them to the
  // list of devices injected into the chroot environment.
  Try<list<string>> nvidia = os::glob("/dev/nvidia*");
  if (nvidia.isError()) {
    return Failure("Failed to glob /dev/nvidia*: " + nvidia.error());
  }

  foreach (const string& device, nvidia.get()) {
    // The directory `/dev/nvidia-caps` was introduced in CUDA 11.0, just
    // ignore it since we only care about the Nvidia GPU device files.
    //
    // TODO(qianzhang): Figure out how to handle the directory
    // `/dev/nvidia-caps` more properly.
    if (device == "/dev/nvidia-caps") {
      continue;
    }

    const string devicePath = path::join(
        devicesDir, strings::remove(device, "/dev/", strings::PREFIX), device);

    Try<Nothing> mknod =
      fs::chroot::copyDeviceNode(device, devicePath);
    if (mknod.isError()) {
      return Failure(
          "Failed to copy device '" + device + "': " + mknod.error());
    }

    // Since we are adding the GPU devices to the container, make
    // them read/write to guarantee that they are accessible inside
    // the container.
    Try<Nothing> chmod = os::chmod(devicePath, 0666);
    if (chmod.isError()) {
      return Failure(
          "Failed to set permissions on device '" + device + "': " +
          chmod.error());
    }

    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        devicePath,
        path::join(containerConfig.rootfs(), device),
        MS_BIND);
  }

  return launchInfo;
}


Future<Nothing> NvidiaGpuIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  Option<double> gpus = resourceRequests.gpus();

  // Make sure that the `gpus` resource is not fractional.
  // We rely on scalar resources only having 3 digits of precision.
  if (static_cast<long long>(gpus.getOrElse(0.0) * 1000.0) % 1000 != 0) {
    return Failure("The 'gpus' resource must be an unsigned integer");
  }

  size_t requested =
    static_cast<size_t>(resourceRequests.gpus().getOrElse(0.0));

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
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

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
  // If we are a nested container, we don't have an `Info()` struct to
  // cleanup, so we just return immediately.
  if (containerId.has_parent()) {
    return Nothing();
  }

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
