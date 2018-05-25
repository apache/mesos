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

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxDevicesIsolatorProcess::create(const Flags& flags)
{
  if (geteuid() != 0) {
    return Error("Linux devices isolator requires root permissions");
  }

  return new MesosIsolator(
      Owned<MesosIsolatorProcess>(new LinuxDevicesIsolatorProcess(flags)));
}


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
    return None();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
