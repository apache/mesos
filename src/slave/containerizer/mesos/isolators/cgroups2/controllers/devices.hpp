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

#ifndef __DEVICES_HPP__
#define __DEVICES_HPP__

#include <string>
#include <vector>

#include <process/owned.hpp>

#include <stout/hashset.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/cgroups2/controller.hpp"
#include "slave/containerizer/device_manager/device_manager.hpp"

namespace mesos {
namespace internal {
namespace slave {

/**
 * Represent cgroups v2 devices controller.
 */
class DeviceControllerProcess: public ControllerProcess
{
public:
  static Try<process::Owned<ControllerProcess>> create(
      const Flags& flags,
      process::Owned<DeviceManager> deviceManager
      );

  ~DeviceControllerProcess() override = default;

  std::string name() const override;

  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const std::string& cgroup,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> recover(
      const ContainerID& containerId,
      const std::string& cgroup) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId,
      const std::string& cgroup) override;

private:
  DeviceControllerProcess(
    const Flags& flags,
    const std::vector<cgroups::devices::Entry>& whitelistDeviceEntries,
    process::Owned<DeviceManager> deviceManager);

  hashset<ContainerID> containerIds;
  std::vector<cgroups::devices::Entry> whitelistDeviceEntries;
  process::Owned<DeviceManager> deviceManager;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DEVICES_HPP__
