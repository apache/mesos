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

#include <utility>

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

Subsystem::Subsystem(Owned<SubsystemProcess> _process)
  : process(std::move(_process))
{
  process::spawn(process.get());
}


Subsystem::~Subsystem()
{
  process::terminate(process.get());
  process::wait(process.get());
}


string Subsystem::name() const
{
  return process->name();
}


Try<Owned<Subsystem>> Subsystem::create(
    const Flags& flags,
    const string& name,
    const string& hierarchy)
{
  hashmap<string, Try<Owned<SubsystemProcess>>(*)(const Flags&, const string&)>
    creators = {
    {CGROUP_SUBSYSTEM_BLKIO_NAME, &BlkioSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_CPU_NAME, &CpuSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_CPUACCT_NAME, &CpuacctSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_CPUSET_NAME, &CpusetSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_DEVICES_NAME, &DevicesSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_HUGETLB_NAME, &HugetlbSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_MEMORY_NAME, &MemorySubsystemProcess::create},
    {CGROUP_SUBSYSTEM_NET_CLS_NAME, &NetClsSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_NET_PRIO_NAME, &NetPrioSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_PERF_EVENT_NAME, &PerfEventSubsystemProcess::create},
    {CGROUP_SUBSYSTEM_PIDS_NAME, &PidsSubsystemProcess::create},
  };

  if (!creators.contains(name)) {
    return Error("Unknown subsystem '" + name + "'");
  }

  Try<Owned<SubsystemProcess>> subsystemProcess =
    creators[name](flags, hierarchy);

  if (subsystemProcess.isError()) {
    return Error(
        "Failed to create subsystem '" + name + "': " +
        subsystemProcess.error());
  }

  return Owned<Subsystem>(new Subsystem(subsystemProcess.get()));
}


Future<Nothing> Subsystem::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::recover,
      containerId,
      cgroup);
}


Future<Nothing> Subsystem::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::prepare,
      containerId,
      cgroup,
      containerConfig);
}


Future<Nothing> Subsystem::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::isolate,
      containerId,
      cgroup,
      pid);
}


Future<mesos::slave::ContainerLimitation> Subsystem::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::watch,
      containerId,
      cgroup);
}


Future<Nothing> Subsystem::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::update,
      containerId,
      cgroup,
      resourceRequests,
      resourceLimits);
}


Future<ResourceStatistics> Subsystem::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::usage,
      containerId,
      cgroup);
}


Future<ContainerStatus> Subsystem::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::status,
      containerId,
      cgroup);
}


Future<Nothing> Subsystem::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &SubsystemProcess::cleanup,
      containerId,
      cgroup);
}


SubsystemProcess::SubsystemProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


Future<Nothing> SubsystemProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}


Future<Nothing> SubsystemProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return Nothing();
}


Future<Nothing> SubsystemProcess::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> SubsystemProcess::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> SubsystemProcess::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return Nothing();
}


Future<ResourceStatistics> SubsystemProcess::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ResourceStatistics();
}


Future<ContainerStatus> SubsystemProcess::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ContainerStatus();
}


Future<Nothing> SubsystemProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
