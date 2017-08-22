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

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/windows/os.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/windows/mem.hpp"

using process::Failure;
using process::Future;

namespace mesos {
namespace internal {
namespace slave {

// Reasonable minimum constraints.
constexpr Bytes MIN_MEM = Megabytes(32);


bool WindowsMemIsolatorProcess::supportsNesting() { return true; }


// When recovering, this ensures that our ContainerID -> PID mapping is
// recreated.
Future<Nothing> WindowsMemIsolatorProcess::recover(
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
WindowsMemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (memLimits.contains(containerId)) {
    return Failure("Container already prepared: " + stringify(containerId));
  }

  Resources resources{containerConfig.resources()};

  if (resources.mem().isSome()) {
    // Save the limit information so that `isolate` can set the limit
    // immediately.
    memLimits[containerId] = std::max(resources.mem().get(), MIN_MEM);
  }

  return None();
}


// This is called when the actual container is launched, and hence has a PID.
// It creates the ContainerID -> PID mapping, and sets the initial memory limit.
Future<Nothing> WindowsMemIsolatorProcess::isolate(
    const ContainerID& containerId, pid_t pid)
{
  if (pids.contains(containerId)) {
    return Failure("Container already isolated: " + stringify(containerId));
  }

  pids.put(containerId, pid);

  if (memLimits.contains(containerId)) {
    Try<Nothing> set =
      os::set_job_mem_limit(pids[containerId], memLimits[containerId]);
    if (set.isError()) {
      return Failure(
          "Failed to isolate container '" + stringify(containerId) +
          "': " + set.error());
    }
  }

  return Nothing();
}


Future<Nothing> WindowsMemIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  pids.erase(containerId);
  memLimits.erase(containerId);

  return Nothing();
}


Try<mesos::slave::Isolator*> WindowsMemIsolatorProcess::create(
    const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(new WindowsMemIsolatorProcess());

  return new MesosIsolator(process);
}


Future<Nothing> WindowsMemIsolatorProcess::update(
    const ContainerID& containerId, const Resources& resources)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!pids.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  if (resources.mem().isNone()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': No mem resource given");
  }

  memLimits[containerId] = std::max(resources.mem().get(), MIN_MEM);
  Try<Nothing> set =
    os::set_job_mem_limit(pids[containerId], memLimits[containerId]);
  if (set.isError()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': " + set.error());
  }

  return Nothing();
}


Future<ResourceStatistics> WindowsMemIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    LOG(WARNING) << "No resource usage for unknown container '" << containerId
                 << "'";

    return ResourceStatistics();
  }

  ResourceStatistics result;

  result.set_timestamp(process::Clock::now().secs());

  const Try<Bytes> mem_total_bytes = os::get_job_mem(pids[containerId]);
  if (mem_total_bytes.isSome()) {
    result.set_mem_total_bytes(mem_total_bytes.get().bytes());
  }

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
