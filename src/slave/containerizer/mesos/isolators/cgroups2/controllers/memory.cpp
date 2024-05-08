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

  oomListen(containerId, cgroup);

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

  oomListen(containerId, cgroup);

  return Nothing();
}


Future<ContainerLimitation> MemoryControllerProcess::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return infos[containerId].limitation.future();
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

  if (infos[containerId].oom.isPending()) {
    infos[containerId].oom.discard();
  }

  infos.erase(containerId);

  return Nothing();
}


void MemoryControllerProcess::oomListen(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    LOG(ERROR) << "Cannot listen for OOM events for unknown container "
               << containerId;
    return;
  }

  infos[containerId].oom = cgroups2::memory::oom(cgroup);

  LOG(INFO) << "Listening for OOM events for container "
            << containerId;

  infos[containerId].oom.onAny(
      defer(PID<MemoryControllerProcess>(this),
            &MemoryControllerProcess::oomed,
            containerId,
            cgroup,
            lambda::_1));
}


void MemoryControllerProcess::oomed(
    const ContainerID& containerId,
    const string& cgroup,
    const Future<Nothing>& oom)
{
  if (oom.isDiscarded()) {
    LOG(INFO) << "OOM event listener discarded";
    return;
  }

  if (oom.isFailed()) {
    LOG(ERROR) << "OOM event listener failed: " << oom.failure();
    return;
  }

  if (!infos.contains(containerId)) {
    // It is likely that process exited is executed before this
    // function (e.g. The kill and OOM events happen at the same time,
    // and the process exit event arrives first). Therefore, we should
    // not report a fatal error here.
    LOG(INFO) << "OOM event received for terminated container";
    return;
  }

  LOG(INFO) << "OOM detected for container" << containerId;

  // Construct a message for the limitation to help with debugging
  // the OOM.

  ostringstream limitMessage;
  limitMessage << "Memory limit exceeded: ";

  // TODO(dleamy): Report the peak memory usage of the container. The
  //               'memory.peak' control is only available on newer Linux
  //               kernels.

  // Report memory statistics.
  limitMessage << "\nMEMORY STATISTICS: \n";
  Try<Stats> memoryStats = cgroups2::memory::stats(cgroup);
  if (memoryStats.isError()) {
    LOG(ERROR) << "Failed to get cgroup memory stats: " << memoryStats.error();
    return;
  }

  limitMessage << "Anon: " << memoryStats->anon << "\n";
  limitMessage << "File: " << memoryStats->file << "\n";
  limitMessage << "Kernel: " << memoryStats->kernel << "\n";
  limitMessage << "Kernel TCP: " << memoryStats->sock << "\n";
  limitMessage << "Kernel Stack: " << memoryStats->kernel_stack << "\n";
  limitMessage << "Page Tables: " << memoryStats->pagetables << "\n";
  limitMessage << "VMalloc: " << memoryStats->vmalloc << "\n";

  // TODO(dleamy): Report 'total_mapped_file' cgroups v2 equivalent.
  //               "The amount of file backed memory, in bytes, mapped into
  //               a process' address space."

  LOG(INFO) << limitMessage.str();

  Result<Bytes> hardLimit = cgroups2::memory::max(cgroup);
  if (hardLimit.isError()) {
    LOG(ERROR) << "Failed to get hard memory limit: " << hardLimit.error();
    return;
  }

  if (hardLimit.isNone()) {
    LOG(ERROR) << "No hard limit was set for the container - unexpected OOM";
    return;
  }

  // Complete the container limitation promise with a memory resource
  // limitation.
  //
  // TODO(jieyu): This is not accurate if the memory resource is from
  // a non-star role or spans roles (e.g., "*" and "role"). Ideally,
  // we should save the resources passed in and report it here.
  //
  // TODO(dleamy): We report the hard limit because not all machines have
  //               access to 'memory.peak', the peak memory usage of the
  //               cgroup.
  double megabytes = (double) hardLimit->bytes() / Bytes::MEGABYTES;
  Resources memory = *Resources::parse("mem", stringify(megabytes), "*");

  infos[containerId].limitation.set(
      protobuf::slave::createContainerLimitation(
          memory,
          limitMessage.str(),
          TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {