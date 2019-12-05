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
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/windows/jobobject.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/windows/mem.hpp"

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
constexpr Bytes MIN_MEM = Megabytes(32);


bool WindowsMemIsolatorProcess::supportsNesting() { return true; }
bool WindowsMemIsolatorProcess::supportsStandalone() { return true; }


// When recovering, this ensures that our ContainerID -> PID mapping is
// recreated.
Future<Nothing> WindowsMemIsolatorProcess::recover(
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


Future<Option<ContainerLaunchInfo>> WindowsMemIsolatorProcess::prepare(
    const ContainerID& containerId, const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container already prepared: " + stringify(containerId));
  }

  infos[containerId] = {};

  const Resources resources = containerConfig.resources();
  if (resources.mem().isSome()) {
    // Save the limit information so that `isolate` can set the limit
    // immediately.
    infos[containerId].limit = max(resources.mem().get(), MIN_MEM);
  }

  return None();
}


// This is called when the actual container is launched, and hence has a PID.
// It creates the ContainerID -> PID mapping, and sets the initial memory limit.
Future<Nothing> WindowsMemIsolatorProcess::isolate(
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
    const Try<Nothing> set = os::set_job_mem_limit(
        infos[containerId].pid.get(), infos[containerId].limit.get());
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
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


Try<Isolator*> WindowsMemIsolatorProcess::create(const Flags& flags)
{
  Owned<MesosIsolatorProcess> process(new WindowsMemIsolatorProcess());

  return new MesosIsolator(process);
}


Future<Nothing> WindowsMemIsolatorProcess::update(
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

  if (resourceRequests.mem().isNone()) {
    return Failure(
        "Failed to update container '" + stringify(containerId) +
        "': No mem resource given");
  }

  infos[containerId].limit = max(resourceRequests.mem().get(), MIN_MEM);
  const Try<Nothing> set = os::set_job_mem_limit(
      infos[containerId].pid.get(), infos[containerId].limit.get());
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

  const Try<Bytes> mem_total_bytes =
    os::get_job_mem(infos[containerId].pid.get());
  if (mem_total_bytes.isSome()) {
    result.set_mem_total_bytes(mem_total_bytes->bytes());
  }

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
