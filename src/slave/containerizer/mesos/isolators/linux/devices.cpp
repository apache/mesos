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

#include "slave/containerizer/mesos/isolators/linux/devices.hpp"

#include <sys/mount.h>

#include <process/id.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/posix/chown.hpp>

#include "slave/containerizer/mesos/paths.hpp"

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerMountInfo;
using mesos::slave::Isolator;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxDevicesIsolatorProcess::create(const Flags& flags)
{
  if (::geteuid() != 0) {
    return Error("Linux devices isolator requires root permissions");
  }

  if (flags.launcher != "linux") {
    return Error("'linux' launcher must be used");
  }

  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' isolator must be used");
  }

  hashmap<string, Device> whitelistedDevices;

  if (flags.allowed_devices.isSome()) {
    foreach (const DeviceAccess& deviceAccess,
             flags.allowed_devices->allowed_devices()) {
      if (!deviceAccess.device().has_path()) {
        return Error("Whitelisted device has no device path provided");
      }

      const string& path = deviceAccess.device().path();

      Try<dev_t> rdev = os::stat::rdev(path);
      if (rdev.isError()) {
        return Error("Failed to obtain device ID for '" + path +
                     "': " + rdev.error());
      }

      Try<mode_t> mode = os::stat::mode(path);
      if (mode.isError()) {
        return Error("Failed to obtain device mode for '" + path +
                     "': " + mode.error());
      }

      Device dev = {rdev.get(), S_IRUSR | S_IWUSR };

      if (S_ISBLK(mode.get())) {
        dev.mode |= S_IFBLK;
      } else if (S_ISCHR(mode.get())) {
        dev.mode |= S_IFCHR;
      } else {
        return Error("'" + path + "' is not a block or character device");
      }

      // Set the desired access for the device. Access is controlled at
      // container granularity, which is consistent with the devices cgroup
      // policy. This means that if we populate a read-write device into a
      // container, then every process in that container should have access,
      // regardless of the credential of that process.

      if (deviceAccess.access().read()) {
        dev.mode |= (S_IRGRP | S_IROTH);
      }

      if (deviceAccess.access().write()) {
        dev.mode |= (S_IWGRP | S_IWOTH);
      }

      whitelistedDevices.put(
          strings::remove(path, "/dev/", strings::PREFIX), dev);
    }
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new LinuxDevicesIsolatorProcess(flags.runtime_dir, whitelistedDevices)));
}

LinuxDevicesIsolatorProcess::LinuxDevicesIsolatorProcess(
    const string& _runtimeDirectory,
    const hashmap<string, Device>& _whitelistedDevices)
  : ProcessBase(process::ID::generate("linux-devices-isolator")),
    runtimeDirectory(_runtimeDirectory),
    whitelistedDevices(_whitelistedDevices) {}


bool LinuxDevicesIsolatorProcess::supportsNesting()
{
  return true;
}


bool LinuxDevicesIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> LinuxDevicesIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // If there's no rootfs, we won't be building a private `/dev`
  // so there's nothing to do.
  if (!containerConfig.has_rootfs()) {
    return None();
  }

  if (whitelistedDevices.empty()) {
    return None();
  }

  ContainerLaunchInfo launchInfo;

  const string devicesDir = containerizer::paths::getContainerDevicesPath(
      runtimeDirectory, containerId);

  Try<Nothing> mkdir = os::mkdir(devicesDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create container devices directory: " + mkdir.error());
  }

  Try<Nothing> chmod = os::chmod(devicesDir, 0700);
  if (chmod.isError()) {
    return Failure("Failed to set container devices directory permissions: " +
                   chmod.error());
  }

  // We need to restrict access to the devices directory so that all
  // processes on the system don't get access to devices that we make
  // read-write. This means that we have to chown to ensure that the
  // container user still has access.
  if (containerConfig.has_user()) {
    Try<Nothing> chown = os::chown(containerConfig.user(), devicesDir);
    if (chown.isError()) {
      return Failure(
          "Failed to set '" + containerConfig.user() + "' "
          "as the container devices directory owner: " + chown.error());
    }
  }

  // Import the whitelisted devices to all containers.
  foreachpair (const string& path, const Device& dev, whitelistedDevices) {
    const string devicePath = path::join(devicesDir, path);

    Try<Nothing> mkdir = os::mkdir(Path(devicePath).dirname());
    if (mkdir.isError()) {
      return Failure(
          "Failed to create parent directory for device '" +
          devicePath + "': " + mkdir.error());
    }

    Try<Nothing> mknod = os::mknod(devicePath, dev.mode, dev.dev);
    if (mknod.isError()) {
      return Failure(
          "Failed to create device '" + devicePath + "': " + mknod.error());
    }

    // We have to chmod the device to make sure that the umask doesn't filter
    // the permissions defined by the whitelist.
    Try<Nothing> chmod = os::chmod(devicePath, dev.mode & ~S_IFMT);
    if (chmod.isError()) {
      return Failure(
          "Failed to chmod device '" + devicePath + "': " + chmod.error());
    }

    ContainerMountInfo* mount = launchInfo.add_mounts();
    mount->set_source(devicePath);
    mount->set_target(path::join(containerConfig.rootfs(), "dev", path));
    mount->set_flags(MS_BIND);
  }

  // TODO(jpeach) Define Task API to let schedulers specify the container
  // devices and automatically populate the right devices cgroup entries.

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
