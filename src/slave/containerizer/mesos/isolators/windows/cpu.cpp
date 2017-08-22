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

#include <algorithm>
#include <list>

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/windows/os.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/windows/cpu.hpp"

using process::Failure;
using process::Future;

namespace mesos {
namespace internal {
namespace slave {

// Reasonable minimum constraints.
constexpr double MIN_CPU = 0.001;


bool WindowsCpuIsolatorProcess::supportsNesting() { return true; }


// When recovering, this ensures that our ContainerID -> PID mapping is
// recreated.
Future<Nothing> WindowsCpuIsolatorProcess::recover(
    const std::list<mesos::slave::ContainerState>& state,
    const hashset<ContainerID>& orphans)
{
  foreach (const mesos::slave::ContainerState& run, state) {
    // This should (almost) never occur: see comment in
    // SubprocessLauncher::recover().
    if (pids.contains(run.container_id())) {
      return Failure("Container already recovered");
    }

    pids.put(run.container_id(), run.pid());
  }

  return Nothing();
}


process::Future<Option<mesos::slave::ContainerLaunchInfo>>
WindowsCpuIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (cpuLimits.contains(containerId)) {
    return Failure("Container already prepared: " + stringify(containerId));
  }

  Resources resources{containerConfig.resources()};
  if (resources.cpus().isSome()) {
    // Save the limit information so that `isolate` can set the limit
    // immediately.
    cpuLimits[containerId] = std::max(resources.cpus().get(), MIN_CPU);
  }

  return None();
}


// This is called when the actual container is launched, and hence has a PID.
// It creates the ContainerID -> PID mapping, and sets the initial CPU limit.
Future<Nothing> WindowsCpuIsolatorProcess::isolate(
    const ContainerID& containerId, pid_t pid)
{
  if (pids.contains(containerId)) {
    return Failure("Container already isolated: " + stringify(containerId));
  }

  pids.put(containerId, pid);

  if (cpuLimits.contains(containerId)) {
    Try<Nothing> set =
      os::set_job_cpu_limit(pids[containerId], cpuLimits[containerId]);
    if (set.isError()) {
      return Failure(
          "Failed to update container '" + stringify(containerId) +
          "': " + set.error());
    }
  }

  return Nothing();
}


Future<Nothing> WindowsCpuIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  pids.erase(containerId);
  cpuLimits.erase(containerId);

  return Nothing();
}


Try<mesos::slave::Isolator*> WindowsCpuIsolatorProcess::create(
    const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(new WindowsCpuIsolatorProcess());

  return new MesosIsolator(process);
}


Future<Nothing> WindowsCpuIsolatorProcess::update(
    const ContainerID& containerId, const Resources& resources)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!pids.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  if (resources.cpus().isNone()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': No cpus resource given");
  }

  cpuLimits[containerId] = std::max(resources.cpus().get(), MIN_CPU);
  Try<Nothing> set =
    os::set_job_cpu_limit(pids[containerId], cpuLimits[containerId]);
  if (set.isError()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': " + set.error());
  }

  return Nothing();
}


Future<ResourceStatistics> WindowsCpuIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    LOG(WARNING) << "No resource usage for unknown container '" << containerId
                 << "'";

    return ResourceStatistics();
  }

  ResourceStatistics result;

  result.set_timestamp(process::Clock::now().secs());

  const Try<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION> info =
    os::get_job_info(pids[containerId]);
  if (info.isError()) {
    return result;
  }

  result.set_processes(info.get().ActiveProcesses);

  // The reported time fields are in 100-nanosecond ticks.
  result.set_cpus_user_time_secs(
      Nanoseconds(info.get().TotalUserTime.QuadPart * 100).secs());

  result.set_cpus_system_time_secs(
      Nanoseconds(info.get().TotalKernelTime.QuadPart * 100).secs());

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
