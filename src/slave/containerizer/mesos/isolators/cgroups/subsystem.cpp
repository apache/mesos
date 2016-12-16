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

#include <stout/error.hpp>
#include <stout/hashmap.hpp>

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystem.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystems/blkio.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/cpu.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/cpuacct.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/cpuset.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/devices.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/hugetlb.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/memory.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/net_prio.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/perf_event.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/pids.hpp"

using mesos::slave::ContainerLimitation;

using process::Future;
using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<Subsystem>> Subsystem::create(
    const Flags& flags,
    const string& name,
    const string& hierarchy)
{
  hashmap<string, Try<Owned<Subsystem>>(*)(const Flags&, const string&)>
    creators = {
    {CGROUP_SUBSYSTEM_BLKIO_NAME, &BlkioSubsystem::create},
    {CGROUP_SUBSYSTEM_CPU_NAME, &CpuSubsystem::create},
    {CGROUP_SUBSYSTEM_CPUACCT_NAME, &CpuacctSubsystem::create},
    {CGROUP_SUBSYSTEM_CPUSET_NAME, &CpusetSubsystem::create},
    {CGROUP_SUBSYSTEM_DEVICES_NAME, &DevicesSubsystem::create},
    {CGROUP_SUBSYSTEM_HUGETLB_NAME, &HugetlbSubsystem::create},
    {CGROUP_SUBSYSTEM_MEMORY_NAME, &MemorySubsystem::create},
    {CGROUP_SUBSYSTEM_NET_CLS_NAME, &NetClsSubsystem::create},
    {CGROUP_SUBSYSTEM_NET_PRIO_NAME, &NetPrioSubsystem::create},
    {CGROUP_SUBSYSTEM_PERF_EVENT_NAME, &PerfEventSubsystem::create},
    {CGROUP_SUBSYSTEM_PIDS_NAME, &PidsSubsystem::create},
  };

  if (!creators.contains(name)) {
    return Error("Unknown subsystem '" + name + "'");
  }

  Try<Owned<Subsystem>> subsystem = creators[name](flags, hierarchy);
  if (subsystem.isError()) {
    return Error(
        "Failed to create subsystem '" + name + "': " +
        subsystem.error());
  }

  return subsystem.get();
}


Subsystem::Subsystem(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


Future<Nothing> Subsystem::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}


Future<Nothing> Subsystem::prepare(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}


Future<Nothing> Subsystem::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> Subsystem::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> Subsystem::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> Subsystem::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ResourceStatistics();
}


Future<ContainerStatus> Subsystem::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ContainerStatus();
}


Future<Nothing> Subsystem::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
