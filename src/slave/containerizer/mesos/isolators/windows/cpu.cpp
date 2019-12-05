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
#include <vector>

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/windows/jobobject.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/windows/cpu.hpp"

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;

using std::max;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// Reasonable minimum constraints.
constexpr double MIN_CPU = 0.001;


bool WindowsCpuIsolatorProcess::supportsNesting() { return true; }
bool WindowsCpuIsolatorProcess::supportsStandalone() { return true; }


// When recovering, this ensures that our ContainerID -> PID mapping is
// recreated.
Future<Nothing> WindowsCpuIsolatorProcess::recover(
    const vector<ContainerState>& state, const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& run, state) {
    // This should (almost) never occur: see comment in
    // SubprocessLauncher::recover().
    if (infos.contains(run.container_id())) {
      return Failure("Container already recovered");
    }

    infos[run.container_id()] = {static_cast<pid_t>(run.pid()), None()};
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> WindowsCpuIsolatorProcess::prepare(
    const ContainerID& containerId, const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container already prepared: " + stringify(containerId));
  }

  infos[containerId] = {};

  const Resources resources = containerConfig.resources();
  if (resources.cpus().isSome()) {
    // Save the limit information so that `isolate` can set the limit
    // immediately.
    infos[containerId].limit = max(resources.cpus().get(), MIN_CPU);
  }

  return None();
}


// This is called when the actual container is launched, and hence has a PID.
// It creates the ContainerID -> PID mapping, and sets the initial CPU limit.
Future<Nothing> WindowsCpuIsolatorProcess::isolate(
    const ContainerID& containerId, pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Container not prepared before isolation: " + stringify(containerId));
  }

  if (infos[containerId].pid.isSome()) {
    return Failure("Container already isolated: " + stringify(containerId));
  }

  infos[containerId].pid = pid;

  if (infos[containerId].limit.isSome()) {
    const Try<Nothing> set = os::set_job_cpu_limit(
        infos[containerId].pid.get(), infos[containerId].limit.get());
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
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


Try<Isolator*> WindowsCpuIsolatorProcess::create(const Flags& flags)
{
  Owned<MesosIsolatorProcess> process(new WindowsCpuIsolatorProcess());

  return new MesosIsolator(process);
}


Future<Nothing> WindowsCpuIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  if (!infos[containerId].pid.isSome()) {
    return Failure(
        "Container not isolated before update: " + stringify(containerId));
  }

  if (resourceRequests.cpus().isNone()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': No cpus resource given");
  }

  infos[containerId].limit = max(resourceRequests.cpus().get(), MIN_CPU);
  const Try<Nothing> set = os::set_job_cpu_limit(
      infos[containerId].pid.get(), infos[containerId].limit.get());
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
  ResourceStatistics result;
  result.set_timestamp(Clock::now().secs());

  if (!infos.contains(containerId)) {
    LOG(WARNING) << "No resource usage for unknown container '" << containerId
                 << "'";

    return result;
  }

  if (!infos[containerId].pid.isSome()) {
    LOG(WARNING) << "No resource usage for container with unknown PID '"
                 << containerId << "'";

    return result;
  }

  const Try<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION> info =
    os::get_job_info(infos[containerId].pid.get());
  if (info.isError()) {
    return result;
  }

  result.set_processes(info->ActiveProcesses);

  // The reported time fields are in 100-nanosecond ticks.
  result.set_cpus_user_time_secs(
      Nanoseconds(info->TotalUserTime.QuadPart * 100).secs());

  result.set_cpus_system_time_secs(
      Nanoseconds(info->TotalKernelTime.QuadPart * 100).secs());

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
