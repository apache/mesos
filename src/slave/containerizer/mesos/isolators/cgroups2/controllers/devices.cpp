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

#include <sys/stat.h>

// This header include must be enclosed in an `extern "C"` block to
// workaround a bug in glibc <= 2.12 (see MESOS-7378).
//
// TODO(neilc): Remove this when we no longer support glibc <= 2.12.
extern "C" {
#include <sys/sysmacros.h>
}

#include <process/defer.hpp>
#include <process/id.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/os.hpp>

#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/devices.hpp"

using cgroups::devices::Entry;

using process::Failure;
using process::Future;
using process::Owned;

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


Try<Owned<ControllerProcess>> DeviceControllerProcess::create(
    const Flags& flags,
    const Owned<DeviceManager> deviceManager)
{
  vector<cgroups::devices::Entry> whitelistDeviceEntries;

  foreach (const char* _entry, DEFAULT_WHITELIST_ENTRIES) {
    whitelistDeviceEntries.push_back(
      CHECK_NOTERROR(cgroups::devices::Entry::parse(_entry)));
  }

  if (flags.allowed_devices.isSome()) {
    foreach (const DeviceAccess& device_access,
             flags.allowed_devices->allowed_devices()) {
      if (!device_access.device().has_path()) {
        return Error("Whitelisted device has no device path provided");
      }

      const string& path = device_access.device().path();
      const DeviceAccess_Access& access = device_access.access();

      if (!(access.read() || access.write() || access.mknod())) {
        return Error("Could not whitelist device '" + path + "'"
                     " without any access privileges");
      }

      Try<dev_t> device = os::stat::rdev(path);
      if (device.isError()) {
        return Error("Failed to obtain device ID for '" + path + "'"
                     ": " + device.error());
      }

      Try<mode_t> mode = os::stat::mode(path);
      if (mode.isError()) {
        return Error("Failed to obtain device mode for '" + path + "'"
                     ": " + mode.error());
      }

      Entry::Selector::Type type;
      if (S_ISBLK(mode.get())) {
        type = Entry::Selector::Type::BLOCK;
      } else if (S_ISCHR(mode.get())) {
        type = Entry::Selector::Type::CHARACTER;
      } else {
        return Error("Failed to determine device type for '" + path + "'");
      }

      cgroups::devices::Entry entry;
      entry.selector.type = type;
      entry.selector.major = major(device.get());
      entry.selector.minor = minor(device.get());
      entry.access.read = access.read();
      entry.access.write = access.write();
      entry.access.mknod = access.mknod();

      whitelistDeviceEntries.push_back(entry);
    }
  }

  return Owned<ControllerProcess>(new DeviceControllerProcess(
      flags, whitelistDeviceEntries, deviceManager));
}


string DeviceControllerProcess::name() const
{
  return CGROUPS2_CONTROLLER_DEVICES_NAME;
}


DeviceControllerProcess::DeviceControllerProcess(
    const Flags& _flags,
    const std::vector<cgroups::devices::Entry>& _whitelistDeviceEntries,
    const Owned<DeviceManager> _deviceManager)
  : ProcessBase(process::ID::generate("cgroups-v2-devices-controller")),
    ControllerProcess(_flags),
    whitelistDeviceEntries(_whitelistDeviceEntries),
    deviceManager(_deviceManager) {}


Future<Nothing> DeviceControllerProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (containerIds.contains(containerId)) {
    return Failure(
        "The controller '" + name() + "' of container " +
        stringify(containerId) + " has already been recovered");
  }

  containerIds.insert(containerId);

  return Nothing();
}


Future<Nothing> DeviceControllerProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (containerIds.contains(containerId)) {
    return Failure("The controller '" + name() + "' has already been prepared");
  }

  return deviceManager->configure(cgroup, whitelistDeviceEntries, {})
    .then(defer(self(), [=] {
      containerIds.insert(containerId);
      return Nothing();
    }));
}


Future<Nothing> DeviceControllerProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!containerIds.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup controller '" << name() << "'"
            << " for unknown container " << containerId;
    return Nothing();
  }

  containerIds.erase(containerId);
  return deviceManager->remove(cgroup);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
