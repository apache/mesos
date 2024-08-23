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

#ifndef __DEVICE_MANAGER_HPP__
#define __DEVICE_MANAGER_HPP__

#include <process/future.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class DeviceManagerProcess;

// Manages cgroups V2 device access state.
//
// In cgroups V2, device control is managed via ebpf programs, as opposed
// to the control files in cgroups V1. As such we lose the ability to
// checkpoint a cgroup's device access state via the control files, and
// we have to checkpoint this information ourselves via this centralized
// device manager.
//
// In addition, we always construct the ebpf programs from the cgroup's entire
// state, which we cannot keep track of via control files anymore. So we need
// the centralized device manager to manage the state and provide an interface
// to incrementally adjust the device access state for a cgroup and generate
// the appropriate ebpf programs.
class DeviceManager
{
public:
  // Used to enforce non-wildcard entry restrictions at compile time.
  struct NonWildcardEntry
  {
    static Try<std::vector<NonWildcardEntry>> create(
        const std::vector<cgroups::devices::Entry>& entries);

    struct Selector
    {
      enum class Type { BLOCK, CHARACTER };
      Type type;
      unsigned int major;
      unsigned int minor;
    };

    Selector selector;
    cgroups::devices::Entry::Access access;
  };

  struct CgroupDeviceAccess
  {
    std::vector<cgroups::devices::Entry> allow_list;
    std::vector<cgroups::devices::Entry> deny_list;

    // A device access is granted if it is encompassed by an allow entry
    // and does not have access overlaps with any deny entry.
    bool is_access_granted(const cgroups::devices::Entry& entry) const;
    bool is_access_granted(const NonWildcardEntry& entry) const;

    // Returns an error if it the allow or deny lists are not normalized.
    static Try<CgroupDeviceAccess> create(
      const std::vector<cgroups::devices::Entry>& allow_list,
      const std::vector<cgroups::devices::Entry>& deny_list);

  private:
    CgroupDeviceAccess(
      const std::vector<cgroups::devices::Entry>& allow_list,
      const std::vector<cgroups::devices::Entry>& deny_list);
  };

  static Try<DeviceManager*> create(const Flags& flags);

  ~DeviceManager();

  // This is used for initial cgroup device access setup.
  // Device type, major number, and minor number wildcards are allowed in
  // allow_list entries, but not in deny_list entries due to implementation
  // complexities.
  process::Future<Nothing> configure(
    const std::string& cgroup,
    const std::vector<cgroups::devices::Entry>& allow,
    const std::vector<NonWildcardEntry>& deny);

  // Modifies the device access settings for a specified cgroup.
  //
  // Wildcards are not allowed in the param entry vectors.
  // The additionals vector contains entries specifying the devices to which
  // access should be granted.
  // The removals vector contains entries specifying the devices to which
  // access should be revoked.
  process::Future<Nothing> reconfigure(
    const std::string& cgroup,
    const std::vector<NonWildcardEntry>& additionals,
    const std::vector<NonWildcardEntry>& removals);

  // Return the cgroup's device access state, which can be
  // used to query if a device access would be granted.
  process::Future<CgroupDeviceAccess> state(const std::string& cgroup) const;

  // Return the cgroup's device access state for all cgroups tracked.
  process::Future<hashmap<std::string, CgroupDeviceAccess>> state() const;

  // Returns cgroup device state with additions and removals applied to it.
  // Exposed for unit testing.
  static DeviceManager::CgroupDeviceAccess apply_diff(
      const DeviceManager::CgroupDeviceAccess& state,
      const std::vector<DeviceManager::NonWildcardEntry>& additions,
      const std::vector<DeviceManager::NonWildcardEntry>& removals);

  // Remove the cgroup from the DeviceManager state if the state contains it.
  process::Future<Nothing> remove(const std::string& cgroup);

  // Recover the cgroup device access state stored in the checkpointing file.
  // We will only recover cgroups that belong to containers in the passed in
  // container states.
  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states);

private:
  explicit DeviceManager(const process::Owned<DeviceManagerProcess>& process);
  process::Owned<DeviceManagerProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DEVICE_MANAGER_HPP__
