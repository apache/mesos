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

#include <sys/types.h>

#include <climits>
#include <cmath>
#include <sstream>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/containerizer/mesos/utils.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystems/memory.hpp"

using cgroups::memory::pressure::Counter;
using cgroups::memory::pressure::Level;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLimitation;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using std::ostringstream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

static const vector<Level> levels()
{
  return {Level::LOW, Level::MEDIUM, Level::CRITICAL};
}


Try<Owned<SubsystemProcess>> MemorySubsystemProcess::create(
    const Flags& flags,
    const string& hierarchy)
{
  // Make sure the kernel OOM-killer is enabled.
  // The Mesos OOM handler, as implemented, is not capable of handling
  // the oom condition by itself safely given the limitations Linux
  // imposes on this code path.
  Try<Nothing> enable = cgroups::memory::oom::killer::enable(
      hierarchy, flags.cgroups_root);

  if (enable.isError()) {
    return Error("Failed to enable kernel OOM killer: " + enable.error());
  }

  // Test if memory pressure listening is enabled. We test that on the
  // root cgroup. We rely on `Counter::create` to test if memory
  // pressure listening is enabled or not. The created counters will
  // be destroyed immediately.
  foreach (const Level& level, levels()) {
    Try<Owned<Counter>> counter = Counter::create(
        hierarchy,
        flags.cgroups_root,
        level);

    if (counter.isError()) {
      return Error(
          "Failed to listen on '" + stringify(level) + "' "
          "memory events: " + counter.error());
    }
  }

  // Determine whether to limit swap or not.
  if (flags.cgroups_limit_swap) {
    Result<Bytes> check = cgroups::memory::memsw_limit_in_bytes(
        hierarchy, flags.cgroups_root);

    if (check.isError()) {
      return Error(
          "Failed to read 'memory.memsw.limit_in_bytes'"
          ": " + check.error());
    } else if (check.isNone()) {
      return Error("'memory.memsw.limit_in_bytes' is not available");
    }
  }

  return Owned<SubsystemProcess>(new MemorySubsystemProcess(flags, hierarchy));
}


MemorySubsystemProcess::MemorySubsystemProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : ProcessBase(process::ID::generate("cgroups-memory-subsystem")),
    SubsystemProcess(_flags, _hierarchy) {}


Future<Nothing> MemorySubsystemProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (infos.contains(containerId)) {
    return Failure("The subsystem '" + name() + "' has already been recovered");
  }

  infos.put(containerId, Owned<Info>(new Info));
  infos[containerId]->hardLimitUpdated = true;

  oomListen(containerId, cgroup);
  pressureListen(containerId, cgroup);

  return Nothing();
}


Future<Nothing> MemorySubsystemProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("The subsystem '" + name() + "' has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info));
  infos[containerId]->hardLimitUpdated = false;
  infos[containerId]->isCommandTask = containerConfig.has_task_info();

  oomListen(containerId, cgroup);
  pressureListen(containerId, cgroup);

  return Nothing();
}


Future<Nothing> MemorySubsystemProcess::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to isolate subsystem '" + name() + "'"
        ": Unknown container");
  }

  // Get the soft limit.
  Try<Bytes> softLimit =
    cgroups::memory::soft_limit_in_bytes(hierarchy, cgroup);

  if (softLimit.isError()) {
    return Failure(
        "Failed to read 'memory.soft_limit_in_bytes'"
        ": " + softLimit.error());
  }

  // Get the hard limit.
  Try<Bytes> hardLimit =
    cgroups::memory::limit_in_bytes(hierarchy, cgroup);

  if (hardLimit.isError()) {
    return Failure(
        "Failed to read 'memory.limit_in_bytes'"
        ": " + hardLimit.error());
  }

  // While the OOM score of a process is a complex function of the process state
  // and configuration, a decent approximation of the OOM score is 10 x percent
  // of memory used by the process + `/proc/$pid/oom_score_adj` (a configurable
  // quantity which is between -1000 and 1000). Containers with higher OOM
  // scores are killed if the system runs out of memory.
  //
  // We would like burstable task containers which consume more memory than
  // their memory requests (i.e. soft limits) to be preferentially OOM-killed
  // first. To accomplish this, we set their OOM score adjustment as shown
  // below, which attempts to ensure that the container which consumes more
  // memory than its memory request will have an OOM score of 1000.
  //
  // Please note that there are two kinds of burstable task containers:
  //   1. Command task containers whose soft limit < hard limit.
  //   2. Nested task containers whose soft limit < hard limit.
  //
  // For any other kinds of containers (see below), we will just leave their OOM
  // score adjustments at the default value (i.e. 0).
  //   1. Containers whose soft limit == hard limit, this is to ensure backward
  //      compatibility.
  //   2. Default executor containers whose soft limit < hard limit.
  //   3. Custom executor containers whose soft limit < hard limit.
  //   4. Debug containers.
  if (softLimit.get() < hardLimit.get() &&
      (infos[containerId]->isCommandTask || containerId.has_parent())) {
    Try<int> oomScoreAdj = calculateOOMScoreAdj(softLimit.get());
    if (oomScoreAdj.isError()) {
      return Failure(
          "Failed to calculate OOM score adjustment: " + oomScoreAdj.error());
    }

    const string oomScoreAdjPath =
      strings::format("/proc/%d/oom_score_adj", pid).get();

    Try<Nothing> write =
      os::write(oomScoreAdjPath, stringify(oomScoreAdj.get()));

    if (write.isError()) {
      return Failure("Failed to set OOM score adjustment: " + write.error());
    }

    LOG(INFO) << "Set " << oomScoreAdjPath << " to " << oomScoreAdj.get()
              << " for container " << containerId;
  }

  return Nothing();
}


Future<ContainerLimitation> MemorySubsystemProcess::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to watch subsystem '" + name() + "'"
        ": Unknown container");
  }

  return infos[containerId]->limitation.future();
}


Future<Nothing> MemorySubsystemProcess::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to update subsystem '" + name() + "'"
        ": Unknown container");
  }

  if (resourceRequests.mem().isNone()) {
    return Failure(
        "Failed to update subsystem '" + name() + "'"
        ": No memory resource given");
  }

  // New limit.
  Bytes mem = resourceRequests.mem().get();
  Bytes softLimit = std::max(mem, MIN_MEMORY);

  // Always set the soft limit.
  Try<Nothing> write = cgroups::memory::soft_limit_in_bytes(
      hierarchy,
      cgroup,
      softLimit);

  if (write.isError()) {
    return Failure(
        "Failed to set 'memory.soft_limit_in_bytes'"
        ": " + write.error());
  }

  LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to "
            << softLimit << " for container " << containerId;

  // Read the existing hard limit.
  Try<Bytes> currentHardLimit =
    cgroups::memory::limit_in_bytes(hierarchy, cgroup);

  if (currentHardLimit.isError()) {
    return Failure(
        "Failed to read 'memory.limit_in_bytes'"
        ": " + currentHardLimit.error());
  }

  Option<double> memLimit = None();
  foreach (auto&& limit, resourceLimits) {
    if (limit.first == "mem") {
      memLimit = limit.second.value();
    }
  }

  // Rather than trying to represent an infinite limit with the `Bytes`
  // type, we represent the infinite case by setting `isInfiniteLimit`
  // to `true` and letting `hardLimit` be NONE.
  bool isInfiniteLimit = false;
  Option<Bytes> hardLimit = None();
  if (memLimit.isSome() && std::isinf(memLimit.get())) {
    isInfiniteLimit = true;
  } else {
    // Set hard limit to memory limit (if any) or to memory request.
    hardLimit = memLimit.isSome() ?
        std::max(Megabytes(static_cast<uint64_t>(memLimit.get())), MIN_MEMORY) :
        softLimit;
  }

  // NOTE: If `flags.cgroups_limit_swap` is (has been) used then both
  // 'limit_in_bytes' and 'memsw.limit_in_bytes' will always be set to
  // the same value.
  bool limitSwap = flags.cgroups_limit_swap;

  auto setLimitInBytes = [=]() -> Try<Nothing> {
    if (isInfiniteLimit) {
      Try<Nothing> write =
        cgroups::write(hierarchy, cgroup, "memory.limit_in_bytes", "-1");

      if (write.isError()) {
        return Error(
            "Failed to update 'memory.limit_in_bytes': " + write.error());
      }

      LOG(INFO) << "Updated 'memory.limit_in_bytes' to -1"
                << " for container " << containerId;
    } else {
      CHECK_SOME(hardLimit);

      Try<Nothing> write = cgroups::memory::limit_in_bytes(
          hierarchy,
          cgroup,
          hardLimit.get());

      if (write.isError()) {
        return Error(
            "Failed to set 'memory.limit_in_bytes'"
            ": " + write.error());
      }

      LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << hardLimit.get()
                << " for container " << containerId;
    }

    return Nothing();
  };

  auto setMemswLimitInBytes = [=]() -> Try<Nothing> {
    if (limitSwap) {
      if (isInfiniteLimit) {
        Try<Nothing> write = cgroups::write(
            hierarchy, cgroup, "memory.memsw.limit_in_bytes", "-1");

        if (write.isError()) {
          return Error(
              "Failed to update 'memory.memsw.limit_in_bytes'"
              ": " + write.error());
        }

        LOG(INFO) << "Updated 'memory.memsw.limit_in_bytes' to -1"
                  << " for container " << containerId;
      } else {
        CHECK_SOME(hardLimit);

        Try<bool> write = cgroups::memory::memsw_limit_in_bytes(
            hierarchy,
            cgroup,
            hardLimit.get());

        if (write.isError()) {
          return Error(
              "Failed to set 'memory.memsw.limit_in_bytes'"
              ": " + write.error());
        }

        LOG(INFO) << "Updated 'memory.memsw.limit_in_bytes' to "
                  << hardLimit.get() << " for container " << containerId;
      }
    }

    return Nothing();
  };

  vector<lambda::function<Try<Nothing>(void)>> setFunctions;

  // Now, determine whether to set the hard limit. We only update the
  // hard limit if this is the first time or when we're raising the
  // existing limit, then we can update the hard limit safely.
  // Otherwise, if we need to decrease 'memory.limit_in_bytes' we may
  // induce an OOM if too much memory is in use. As a result, we only
  // update the soft limit when the memory reservation is being
  // reduced. This is probably okay if the machine has available
  // resources.
  //
  // TODO(benh): Introduce a MemoryWatcherProcess which monitors the
  // discrepancy between usage and soft limit and introduces a "manual
  // oom" if necessary.
  //
  // NOTE: It's required by the Linux kernel that
  // 'memory.limit_in_bytes' should be less than or equal to
  // 'memory.memsw.limit_in_bytes'. Otherwise, the kernel will fail
  // the cgroup write with EINVAL. As a result, the order of setting
  // these two control files is important. See MESOS-7237 for details.
  if (!infos[containerId]->hardLimitUpdated) {
    // This is the first time memory hard limit is being set. So
    // effectively we are reducing the memory limits because of which
    // we need to set the 'memory.limit_in_bytes' before setting
    // 'memory.memsw.limit_in_bytes'
    setFunctions = {setLimitInBytes, setMemswLimitInBytes};
  } else if (isInfiniteLimit || hardLimit.get() > currentHardLimit.get()) {
    setFunctions = {setMemswLimitInBytes, setLimitInBytes};
  }

  foreach (const auto& setFunction, setFunctions) {
    Try<Nothing> result = setFunction();
    if (result.isError()) {
      return Failure(result.error());
    }
  }

  infos[containerId]->hardLimitUpdated = true;

  return Nothing();
}


Future<ResourceStatistics> MemorySubsystemProcess::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to get usage for subsystem '" + name() + "'"
        ": Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  ResourceStatistics result;

  // The rss from memory.stat is wrong in two dimensions:
  //   1. It does not include child cgroups.
  //   2. It does not include any file backed pages.
  Try<Bytes> usage = cgroups::memory::usage_in_bytes(hierarchy, cgroup);

  if (usage.isError()) {
    return Failure("Failed to parse 'memory.usage_in_bytes': " + usage.error());
  }

  result.set_mem_total_bytes(usage->bytes());

  if (flags.cgroups_limit_swap) {
    Try<Bytes> usage = cgroups::memory::memsw_usage_in_bytes(hierarchy, cgroup);

    if (usage.isError()) {
      return Failure(
        "Failed to parse 'memory.memsw.usage_in_bytes': " + usage.error());
    }

    result.set_mem_total_memsw_bytes(usage->bytes());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g, cgroups::memory::stat.
  Try<hashmap<string, uint64_t>> stat = cgroups::stat(
      hierarchy,
      cgroup,
      "memory.stat");

  if (stat.isError()) {
    return Failure("Failed to read 'memory.stat': " + stat.error());
  }

  Option<uint64_t> total_cache = stat->get("total_cache");
  if (total_cache.isSome()) {
    // TODO(chzhcn): mem_file_bytes is deprecated in 0.23.0 and will
    // be removed in 0.24.0.
    result.set_mem_file_bytes(total_cache.get());
    result.set_mem_cache_bytes(total_cache.get());
  }

  Option<uint64_t> total_rss = stat->get("total_rss");
  if (total_rss.isSome()) {
    // TODO(chzhcn): mem_anon_bytes is deprecated in 0.23.0 and will
    // be removed in 0.24.0.
    result.set_mem_anon_bytes(total_rss.get());
    result.set_mem_rss_bytes(total_rss.get());
  }

  Option<uint64_t> total_mapped_file = stat->get("total_mapped_file");
  if (total_mapped_file.isSome()) {
    result.set_mem_mapped_file_bytes(total_mapped_file.get());
  }

  Option<uint64_t> total_swap = stat->get("total_swap");
  if (total_swap.isSome()) {
    result.set_mem_swap_bytes(total_swap.get());
  }

  Option<uint64_t> total_unevictable = stat->get("total_unevictable");
  if (total_unevictable.isSome()) {
    result.set_mem_unevictable_bytes(total_unevictable.get());
  }

  // Get pressure counter readings.
  vector<Level> levels;
  vector<Future<uint64_t>> values;
  foreachpair (Level level,
               const Owned<Counter>& counter,
               info->pressureCounters) {
    levels.push_back(level);
    values.push_back(counter->value());
  }

  return await(values)
    .then(defer(PID<MemorySubsystemProcess>(this),
                &MemorySubsystemProcess::_usage,
                containerId,
                result,
                levels,
                lambda::_1));
}


Future<ResourceStatistics> MemorySubsystemProcess::_usage(
    const ContainerID& containerId,
    ResourceStatistics result,
    const vector<Level>& levels,
    const vector<Future<uint64_t>>& values)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to get usage for subsystem '" + name() + "'"
        ": Unknown container");
  }

  vector<Level>::const_iterator iterator = levels.begin();
  foreach (const Future<uint64_t>& value, values) {
    if (value.isReady()) {
      switch (*iterator) {
        case Level::LOW:
          result.set_mem_low_pressure_counter(value.get());
          break;
        case Level::MEDIUM:
          result.set_mem_medium_pressure_counter(value.get());
          break;
        case Level::CRITICAL:
          result.set_mem_critical_pressure_counter(value.get());
          break;
      }
    } else {
      LOG(ERROR) << "Failed to listen on '" << stringify(*iterator)
                 << "' pressure events for container " << containerId << ": "
                 << (value.isFailed() ? value.failure() : "discarded");
    }

    ++iterator;
  }

  return result;
}


Future<Nothing> MemorySubsystemProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup subsystem '" << name() << "' "
            << "request for unknown container " << containerId;

    return Nothing();
  }

  if (infos[containerId]->oomNotifier.isPending()) {
    infos[containerId]->oomNotifier.discard();
  }

  infos.erase(containerId);

  return Nothing();
}


void MemorySubsystemProcess::oomListen(
    const ContainerID& containerId,
    const string& cgroup)
{
  CHECK(infos.contains(containerId));

  const Owned<Info>& info = infos[containerId];

  info->oomNotifier = cgroups::memory::oom::listen(hierarchy, cgroup);

  // If the listening fails immediately, something very wrong
  // happened. Therefore, we report a fatal error here.
  if (info->oomNotifier.isFailed()) {
    LOG(FATAL) << "Failed to listen for OOM events for container "
               << containerId << ": "
               << info->oomNotifier.failure();
  }

  LOG(INFO) << "Started listening for OOM events for container "
            << containerId;

  info->oomNotifier.onAny(
      defer(PID<MemorySubsystemProcess>(this),
            &MemorySubsystemProcess::oomWaited,
            containerId,
            cgroup,
            lambda::_1));
}


void MemorySubsystemProcess::oomWaited(
    const ContainerID& containerId,
    const string& cgroup,
    const Future<Nothing>& future)
{
  if (future.isDiscarded()) {
    LOG(INFO) << "Discarded OOM notifier for container " << containerId;
    return;
  }

  if (future.isFailed()) {
    LOG(ERROR) << "Listening on OOM events failed for container "
               << containerId << ": " << future.failure();
    return;
  }

  if (!infos.contains(containerId)) {
    // It is likely that process exited is executed before this
    // function (e.g. The kill and OOM events happen at the same time,
    // and the process exit event arrives first). Therefore, we should
    // not report a fatal error here.
    LOG(INFO) << "OOM detected for the terminated container " << containerId;
    return;
  }

  LOG(INFO) << "OOM detected for container " << containerId;

  // Construct a "message" string to describe why the isolator
  // destroyed the container's cgroup (in order to assist debugging).
  ostringstream message;
  message << "Memory limit exceeded: ";

  // Output the requested memory limit.
  // NOTE: If 'flags.cgroups_limit_swap' is (has been) used, then both
  // 'limit_in_bytes' and 'memsw.limit_in_bytes' will always be set to
  // the same value.
  Try<Bytes> limit = cgroups::memory::limit_in_bytes(hierarchy, cgroup);

  if (limit.isError()) {
    LOG(ERROR) << "Failed to read 'memory.limit_in_bytes': " << limit.error();
  } else {
    message << "Requested: " << limit.get() << " ";
  }

  // Output the maximum memory usage.
  Try<Bytes> usage = cgroups::memory::max_usage_in_bytes(hierarchy, cgroup);

  if (usage.isError()) {
    LOG(ERROR) << "Failed to read 'memory.max_usage_in_bytes': "
               << usage.error();
  } else {
    message << "Maximum Used: " << usage.get() << "\n";
  }

  // Output 'memory.stat' of the cgroup to help with debugging.
  // NOTE: With kernel OOM-killer enabled these stats may not reflect
  // memory state at time of OOM.
  Try<string> read = cgroups::read(hierarchy, cgroup, "memory.stat");

  if (read.isError()) {
    LOG(ERROR) << "Failed to read 'memory.stat': " << read.error();
  } else {
    message << "\nMEMORY STATISTICS: \n" << read.get() << "\n";
  }

  LOG(INFO) << strings::trim(message.str()); // Trim the extra '\n' at the end.

  // TODO(jieyu): This is not accurate if the memory resource is from
  // a non-star role or spans roles (e.g., "*" and "role"). Ideally,
  // we should save the resources passed in and report it here.
  Resources mem = Resources::parse(
      "mem",
      stringify(usage.isSome()
        ? (double) usage->bytes() / Bytes::MEGABYTES : 0),
      "*").get();

  infos[containerId]->limitation.set(
      protobuf::slave::createContainerLimitation(
          mem,
          message.str(),
          TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY));
}


void MemorySubsystemProcess::pressureListen(
    const ContainerID& containerId,
    const string& cgroup)
{
  CHECK(infos.contains(containerId));

  foreach (const Level& level, levels()) {
    Try<Owned<Counter>> counter = Counter::create(hierarchy, cgroup, level);

    if (counter.isError()) {
      LOG(ERROR) << "Failed to listen on '" << level << "' memory pressure "
                 << "events for container " << containerId << ": "
                 << counter.error();
    } else {
      infos[containerId]->pressureCounters[level] = counter.get();

      LOG(INFO) << "Started listening on '" << level << "' memory pressure "
                << "events for container " << containerId;
    }
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
