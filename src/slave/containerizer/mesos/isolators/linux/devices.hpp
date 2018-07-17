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

#ifndef __LINUX_DEVICES_ISOLATOR_HPP__
#define __LINUX_DEVICES_ISOLATOR_HPP__

#include <sys/types.h>

#include <string>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

class LinuxDevicesIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

private:
  struct Device {
    dev_t dev;
    mode_t mode;
  };

  const std::string runtimeDirectory;
  const hashmap<std::string, Device> whitelistedDevices;

  LinuxDevicesIsolatorProcess(
      const std::string& runtimeDirectory,
      const hashmap<std::string, Device>& whitelistedDevices);
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif  // __LINUX_DEVICES_ISOLATOR_HPP__
