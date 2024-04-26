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

#include <sstream>

#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>

#include <stout/bytes.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/cgroups2.hpp"

#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/memory.hpp"

using process::Failure;
using process::Future;
using process::PID;
using process::Owned;

using cgroups2::memory::Stats;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLimitation;

using std::ostringstream;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<ControllerProcess>> MemoryControllerProcess::create(const Flags& flags)
{
  return Owned<ControllerProcess>(new MemoryControllerProcess(flags));
}


MemoryControllerProcess::MemoryControllerProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("cgroups-v2-memory-controller")),
    ControllerProcess(_flags) {}


string MemoryControllerProcess::name() const
{
  return CGROUPS_V2_CONTROLLER_MEMORY_NAME;
}


Future<Nothing> MemoryControllerProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Already prepared");
  }

  infos.put(containerId, Info());

  return Nothing();
}


Future<Nothing> MemoryControllerProcess::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  // TODO(dleamy): Implement manual OOM score adjustment, similar to as it done
  //               in the cgroups v1 isolator.

  return Nothing();
}


Future<Nothing> MemoryControllerProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (infos.contains(containerId)) {
    return Failure("Already recovered");
  }

  infos.put(containerId, Info());
  infos[containerId].hardLimitUpdated = true;

  return Nothing();
}


Future<Nothing> MemoryControllerProcess::update(
  const ContainerID& containerId,
  const string& cgroup,
  const Resources& resourceRequests,
  const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  if (resourceRequests.mem().isNone()) {
    return Failure("No memory resources requested");
  }

  Bytes memory = *resourceRequests.mem();
  Bytes softLimit = std::max(memory, MIN_MEMORY);

  // Set the soft memory limit.
  Try<Nothing> high = cgroups2::memory::set_high(cgroup, softLimit);
  if (high.isError()) {
    return Failure("Failed to set soft memory limit: " + high.error());
  }

  LOG(INFO) << "Updated soft memory limit to " << softLimit << " for container "
            << containerId;

  // Determine the new hard memory limit.
  Option<Bytes> newHardLimit = [&resourceLimits, &softLimit]() -> Option<Bytes>
  {
    if (resourceLimits.count("mem") > 0) {
      double requestedLimit = resourceLimits.at("mem").value();
      if (std::isinf(requestedLimit)) {
        return None();
      }

      return std::max(
          Megabytes(static_cast<uint64_t>(requestedLimit)), MIN_MEMORY);
    }

    return softLimit;
  }();

  Result<Bytes> currentHardLimit = cgroups2::memory::max(cgroup);
  if (currentHardLimit.isError()) {
    return Failure("Failed to get current hard memory limit: "
                   + currentHardLimit.error());
  }

  // We only update the hard limit if:
  // 1) The hard limit has not yet been set for the container, or
  // 2) The new hard limit is greater than the existing hard limit.
  //
  // This is done to avoid the chance of triggering an OOM by reducing the
  // hard limit to below the current memory usage.

  bool updateHardLimit = !infos[containerId].hardLimitUpdated
    || newHardLimit.isNone() // infinite memory limit
    || *newHardLimit > *currentHardLimit;

  if (updateHardLimit) {
    Try<Nothing> max = cgroups2::memory::set_max(cgroup, newHardLimit);
    if (max.isError()) {
      return Failure("Failed to set hard memory limit: " + max.error());
    }

    infos[containerId].hardLimitUpdated = true;
  }

  return Nothing();
}


Future<Nothing> MemoryControllerProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring memory cleanup for unknown container "
              << containerId;

    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
