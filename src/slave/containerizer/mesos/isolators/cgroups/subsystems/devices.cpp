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

#include <process/id.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystems/devices.hpp"

using cgroups::devices::Entry;

using process::Failure;
using process::Future;
using process::Owned;

using std::string;

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


Try<Owned<Subsystem>> DevicesSubsystem::create(
    const Flags& flags,
    const string& hierarchy)
{
  return Owned<Subsystem>(new DevicesSubsystem(flags, hierarchy));
}


DevicesSubsystem::DevicesSubsystem(
    const Flags& _flags,
    const string& _hierarchy)
  : ProcessBase(process::ID::generate("cgroups-devices-subsystem")),
    Subsystem(_flags, _hierarchy) {}


Future<Nothing> DevicesSubsystem::recover(const ContainerID& containerId)
{
  if (containerIds.contains(containerId)) {
    return Failure(
        "The subsystem '" + name() + "' of container " +
        stringify(containerId) + " has already been recovered");
  }

  containerIds.insert(containerId);

  return Nothing();
}


Future<Nothing> DevicesSubsystem::prepare(const ContainerID& containerId)
{
  if (containerIds.contains(containerId)) {
    return Failure("The subsystem '" + name() + "' has already been prepared");
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

  Try<Nothing> deny = cgroups::devices::deny(
      hierarchy,
      path::join(flags.cgroups_root, containerId.value()),
      all);

  if (deny.isError()) {
    return Failure("Failed to deny all devices: " + deny.error());
  }

  foreach (const char* _entry, DEFAULT_WHITELIST_ENTRIES) {
    Try<cgroups::devices::Entry> entry =
      cgroups::devices::Entry::parse(_entry);

    CHECK_SOME(entry);

    Try<Nothing> allow = cgroups::devices::allow(
        hierarchy, path::join(flags.cgroups_root, containerId.value()),
        entry.get());

    if (allow.isError()) {
      return Failure("Failed to whitelist default device "
                     "'" + stringify(entry.get()) + "': " + allow.error());
    }
  }

  containerIds.insert(containerId);

  return Nothing();
}


Future<Nothing> DevicesSubsystem::cleanup(const ContainerID& containerId)
{
  if (!containerIds.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup subsystem '" << name() << "' "
            << "for unknown container " << containerId;

    return Nothing();
  }

  containerIds.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
