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

#include <list>
#include <string>

#include <process/future.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/devices/gpus/nvidia.hpp"

using namespace process;

using std::list;
using std::string;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

CgroupsNvidiaGpuIsolatorProcess::~CgroupsNvidiaGpuIsolatorProcess() {}


Try<Isolator*> CgroupsNvidiaGpuIsolatorProcess::create(const Flags& flags)
{
  return Error("Cgroups Nvidia GPU isolation currently not supported");
}


Future<Nothing> CgroupsNvidiaGpuIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<Option<ContainerLaunchInfo>> CgroupsNvidiaGpuIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<Nothing> CgroupsNvidiaGpuIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<ContainerLimitation> CgroupsNvidiaGpuIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<Nothing> CgroupsNvidiaGpuIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<ResourceStatistics> CgroupsNvidiaGpuIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


Future<Nothing> CgroupsNvidiaGpuIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Failure("Cgroups Nvidia GPU isolation currently not supported");
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
