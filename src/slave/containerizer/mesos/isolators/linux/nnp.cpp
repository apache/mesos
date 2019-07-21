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

#include <stout/try.hpp>

#include "common/kernel_version.hpp"

#include "slave/containerizer/mesos/isolators/linux/nnp.hpp"

using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxNNPIsolatorProcess::create(const Flags& flags)
{
  // PR_SET_NO_NEW_PRIVS requires Linux kernel version greater than or
  // equal to 3.5.
  Try<Version> version = mesos::kernelVersion();

  if (version.isError()) {
    return Error("Could not determine kernel version");
  }

  if (version.get() < Version(3, 5, 0)) {
    return Error("Linux kernel version greater than or equal to 3.5 required");
  }

  return new MesosIsolator(
      Owned<MesosIsolatorProcess>(new LinuxNNPIsolatorProcess()));
}


bool LinuxNNPIsolatorProcess::supportsNesting()
{
  return true;
}


bool LinuxNNPIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> LinuxNNPIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  ContainerLaunchInfo launchInfo;

  launchInfo.set_no_new_privileges(true);

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
