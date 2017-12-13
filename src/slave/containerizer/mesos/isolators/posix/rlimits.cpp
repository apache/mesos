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

#include <process/owned.hpp>

#include "slave/containerizer/mesos/isolators/posix/rlimits.hpp"

using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

bool PosixRLimitsIsolatorProcess::supportsNesting()
{
  return true;
}


bool PosixRLimitsIsolatorProcess::supportsStandalone()
{
  return true;
}


process::Future<Option<ContainerLaunchInfo>>
PosixRLimitsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  Option<RLimitInfo> rlimits = None();

  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_rlimit_info()) {
    rlimits = containerConfig.container_info().rlimit_info();
  }

  if (rlimits.isNone()) {
    return None();
  }

  ContainerLaunchInfo launchInfo;
  launchInfo.mutable_rlimits()->CopyFrom(rlimits.get());

  return launchInfo;
}


Try<Isolator*> PosixRLimitsIsolatorProcess::create(const Flags& flags)
{
  return new MesosIsolator(
      process::Owned<MesosIsolatorProcess>(
          new PosixRLimitsIsolatorProcess(flags)));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
