/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <list>
#include <vector>

#include <mesos/type_utils.hpp>
#include <mesos/values.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/pid.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/containerizer/isolators/cgroups/constants.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"

using namespace process;

using cgroups::memory::pressure::Level;
using cgroups::memory::pressure::Counter;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerPrepareInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

static const vector<Level> levels()
{
  return {Level::LOW, Level::MEDIUM, Level::CRITICAL};
}


CgroupsMemIsolatorProcess::CgroupsMemIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy,
    const bool _limitSwap)
  : flags(_flags),
    hierarchy(_hierarchy),
    limitSwap(_limitSwap) {}


CgroupsMemIsolatorProcess::~CgroupsMemIsolatorProcess() {}


Try<Isolator*> CgroupsMemIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "memory",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create memory cgroup: " + hierarchy.error());
  }

  // Ensure that no other subsystem is attached to the hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(hierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        hierarchy.get());
  } else if (subsystems.get().size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        hierarchy.get());
  }

  // Make sure the kernel OOM-killer is enabled.
  // The Mesos OOM handler, as implemented, is not capable of handling
  // the oom condition by itself safely given the limitations Linux
  // imposes on this code path.
  Try<Nothing> enable = cgroups::memory::oom::killer::enable(
      hierarchy.get(), flags.cgroups_root);

  if (enable.isError()) {
    return Error(enable.error());
  }

  // Test if memory pressure listening is enabled. We test that on the
  // root cgroup. We rely on 'Counter::create' to test if memory
  // pressure listening is enabled or not. The created counters will
  // be destroyed immediately.
  foreach (Level level, levels()) {
    Try<Owned<Counter>> counter = Counter::create(
        hierarchy.get(),
        flags.cgroups_root,
        level);

    if (counter.isError()) {
      return Error("Failed to listen on " + stringify(level) +
                   " memory events: " + counter.error());
    }
  }

  // Determine whether to limit swap or not.
  bool limitSwap = false;

  if (flags.cgroups_limit_swap) {
    Result<Bytes> check = cgroups::memory::memsw_limit_in_bytes(
        hierarchy.get(), flags.cgroups_root);

    if (check.isError()) {
      return Error(
          "Failed to read 'memory.memsw.limit_in_bytes': " +
          check.error());
    } else if (check.isNone()) {
      return Error("'memory.memsw.limit_in_bytes' is not available");
    }

    limitSwap = true;
  }

  process::Owned<MesosIsolatorProcess> process(
      new CgroupsMemIsolatorProcess(flags, hierarchy.get(), limitSwap));

  return new MesosIsolator(process);
}


Future<Nothing> CgroupsMemIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("Failed to check cgroup for container '" +
                     stringify(containerId) + "'");
    }

    if (!exists.get()) {
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);

    oomListen(containerId);
    pressureListen(containerId);
  }

  // Remove orphan cgroups.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
    foreachvalue (Info* info, infos) {
      delete info;
    }
    infos.clear();
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Ignore the slave cgroup (see the --slave_subsystems flag).
    // TODO(idownes): Remove this when the cgroups layout is updated,
    // see MESOS-1185.
    if (cgroup == path::join(flags.cgroups_root, "slave")) {
      continue;
    }

    ContainerID containerId;
    containerId.set_value(Path(cgroup).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    // Known orphan cgroups will be destroyed by the containerizer
    // using the normal cleanup path. See MESOS-2367 for details.
    if (orphans.contains(containerId)) {
      infos[containerId] = new Info(containerId, cgroup);
      continue;
    }

    LOG(INFO) << "Removing unknown orphaned cgroup '" << cgroup << "'";

    // We don't wait on the destroy as we don't want to block recovery.
    cgroups::destroy(hierarchy, cgroup, cgroups::DESTROY_TIMEOUT);
  }

  return Nothing();
}


Future<Option<ContainerPrepareInfo>> CgroupsMemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // TODO(bmahler): Don't insert into 'infos' unless we create the
  // cgroup successfully. It's safe for now because 'cleanup' gets
  // called if we return a Failure, but cleanup will fail because the
  // cgroup does not exist when cgroups::destroy is called.
  Info* info = new Info(
      containerId, path::join(flags.cgroups_root, containerId.value()));

  infos[containerId] = info;

  // Create a cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchy, info->cgroup);

  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  } else if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  Try<Nothing> create = cgroups::create(hierarchy, info->cgroup);
  if (create.isError()) {
    return Failure("Failed to prepare isolator: " + create.error());
  }

  // Chown the cgroup so the executor can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  if (user.isSome()) {
    Try<Nothing> chown = os::chown(
        user.get(),
        path::join(hierarchy, info->cgroup),
        false);
    if (chown.isError()) {
      return Failure("Failed to prepare isolator: " + chown.error());
    }
  }

  oomListen(containerId);
  pressureListen(containerId);

  return update(containerId, executorInfo.resources())
    .then([]() -> Future<Option<ContainerPrepareInfo>> {
      return None();
    });
}


Future<Nothing> CgroupsMemIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  CHECK_NONE(info->pid);
  info->pid = pid;

  Try<Nothing> assign = cgroups::assign(hierarchy, info->cgroup, pid);
  if (assign.isError()) {
    return Failure("Failed to assign container '" +
                   stringify(info->containerId) + "' to its own cgroup '" +
                   path::join(hierarchy, info->cgroup) +
                   "' : " + assign.error());
  }

  return Nothing();
}


Future<ContainerLimitation> CgroupsMemIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  CHECK_NOTNULL(infos[containerId]);

  return infos[containerId]->limitation.future();
}


Future<Nothing> CgroupsMemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (resources.mem().isNone()) {
    return Failure("No memory resource given");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  // New limit.
  Bytes mem = resources.mem().get();
  Bytes limit = std::max(mem, MIN_MEMORY);

  // Always set the soft limit.
  Try<Nothing> write =
    cgroups::memory::soft_limit_in_bytes(hierarchy, info->cgroup, limit);

  if (write.isError()) {
    return Failure(
        "Failed to set 'memory.soft_limit_in_bytes': " + write.error());
  }

  LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to " << limit
            << " for container " << containerId;

  // Read the existing limit.
  Try<Bytes> currentLimit =
    cgroups::memory::limit_in_bytes(hierarchy, info->cgroup);

  // NOTE: If limitSwap is (has been) used then both limit_in_bytes
  // and memsw.limit_in_bytes will always be set to the same value.
  if (currentLimit.isError()) {
    return Failure(
        "Failed to read 'memory.limit_in_bytes': " + currentLimit.error());
  }

  // Determine whether to set the hard limit. If this is the first
  // time (info->pid.isNone()), or we're raising the existing limit,
  // then we can update the hard limit safely. Otherwise, if we need
  // to decrease 'memory.limit_in_bytes' we may induce an OOM if too
  // much memory is in use. As a result, we only update the soft limit
  // when the memory reservation is being reduced. This is probably
  // okay if the machine has available resources.
  // TODO(benh): Introduce a MemoryWatcherProcess which monitors the
  // discrepancy between usage and soft limit and introduces a "manual
  // oom" if necessary.
  if (info->pid.isNone() || limit > currentLimit.get()) {
    // We always set limit_in_bytes first and optionally set
    // memsw.limit_in_bytes if limitSwap is true.
    Try<Nothing> write = cgroups::memory::limit_in_bytes(
        hierarchy, info->cgroup, limit);

    if (write.isError()) {
      return Failure(
          "Failed to set 'memory.limit_in_bytes': " + write.error());
    }

    LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << limit
              << " for container " << containerId;

    if (limitSwap) {
      Try<bool> write = cgroups::memory::memsw_limit_in_bytes(
          hierarchy, info->cgroup, limit);

      if (write.isError()) {
        return Failure(
            "Failed to set 'memory.memsw.limit_in_bytes': " + write.error());
      }

      LOG(INFO) << "Updated 'memory.memsw.limit_in_bytes' to " << limit
                << " for container " << containerId;
    }
  }

  return Nothing();
}


Future<ResourceStatistics> CgroupsMemIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  ResourceStatistics result;

  // The rss from memory.stat is wrong in two dimensions:
  //   1. It does not include child cgroups.
  //   2. It does not include any file backed pages.
  Try<Bytes> usage = cgroups::memory::usage_in_bytes(hierarchy, info->cgroup);
  if (usage.isError()) {
    return Failure("Failed to parse memory.usage_in_bytes: " + usage.error());
  }

  result.set_mem_total_bytes(usage.get().bytes());

  if (limitSwap) {
    Try<Bytes> usage =
      cgroups::memory::memsw_usage_in_bytes(hierarchy, info->cgroup);
    if (usage.isError()) {
      return Failure(
        "Failed to parse memory.memsw.usage_in_bytes: " + usage.error());
    }

    result.set_mem_total_memsw_bytes(usage.get().bytes());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g, cgroups::memory::stat.
  Try<hashmap<string, uint64_t>> stat =
    cgroups::stat(hierarchy, info->cgroup, "memory.stat");
  if (stat.isError()) {
    return Failure("Failed to read memory.stat: " + stat.error());
  }

  Option<uint64_t> total_cache = stat.get().get("total_cache");
  if (total_cache.isSome()) {
    // TODO(chzhcn): mem_file_bytes is deprecated in 0.23.0 and will
    // be removed in 0.24.0.
    result.set_mem_file_bytes(total_cache.get());

    result.set_mem_cache_bytes(total_cache.get());
  }

  Option<uint64_t> total_rss = stat.get().get("total_rss");
  if (total_rss.isSome()) {
    // TODO(chzhcn): mem_anon_bytes is deprecated in 0.23.0 and will
    // be removed in 0.24.0.
    result.set_mem_anon_bytes(total_rss.get());

    result.set_mem_rss_bytes(total_rss.get());
  }

  Option<uint64_t> total_mapped_file = stat.get().get("total_mapped_file");
  if (total_mapped_file.isSome()) {
    result.set_mem_mapped_file_bytes(total_mapped_file.get());
  }

  Option<uint64_t> total_swap = stat.get().get("total_swap");
  if (total_swap.isSome()) {
    result.set_mem_swap_bytes(total_swap.get());
  }

  Option<uint64_t> total_unevictable = stat.get().get("total_unevictable");
  if (total_unevictable.isSome()) {
    result.set_mem_unevictable_bytes(total_unevictable.get());
  }

  // Get pressure counter readings.
  list<Level> levels;
  list<Future<uint64_t>> values;
  foreachpair (Level level,
               const Owned<Counter>& counter,
               info->pressureCounters) {
    levels.push_back(level);
    values.push_back(counter->value());
  }

  return await(values)
    .then(defer(PID<CgroupsMemIsolatorProcess>(this),
                &CgroupsMemIsolatorProcess::_usage,
                containerId,
                result,
                levels,
                lambda::_1));
}


Future<ResourceStatistics> CgroupsMemIsolatorProcess::_usage(
    const ContainerID& containerId,
    ResourceStatistics result,
    const list<Level>& levels,
    const list<Future<uint64_t>>& values)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Level>::const_iterator iterator = levels.begin();
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
      LOG(ERROR) << "Failed to listen on " << stringify(*iterator)
                 << " pressure events for container " << containerId << ": "
                 << (value.isFailed() ? value.failure() : "discarded");
    }

    ++iterator;
  }

  return result;
}


Future<Nothing> CgroupsMemIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;
    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (info->oomNotifier.isPending()) {
    info->oomNotifier.discard();
  }

  return cgroups::destroy(hierarchy, info->cgroup, cgroups::DESTROY_TIMEOUT)
    .onAny(defer(PID<CgroupsMemIsolatorProcess>(this),
                 &CgroupsMemIsolatorProcess::_cleanup,
                 containerId,
                 lambda::_1));
}


Future<Nothing> CgroupsMemIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  CHECK_NOTNULL(infos[containerId]);

  if (!future.isReady()) {
    return Failure("Failed to clean up container " + stringify(containerId) +
                   " : " + (future.isFailed() ? future.failure()
                                              : "discarded"));
  }

  delete infos[containerId];
  infos.erase(containerId);

  return Nothing();
}


void CgroupsMemIsolatorProcess::oomListen(
    const ContainerID& containerId)
{
  CHECK(infos.contains(containerId));
  Info* info = CHECK_NOTNULL(infos[containerId]);

  info->oomNotifier = cgroups::memory::oom::listen(hierarchy, info->cgroup);

  // If the listening fails immediately, something very wrong
  // happened.  Therefore, we report a fatal error here.
  if (info->oomNotifier.isFailed()) {
    LOG(FATAL) << "Failed to listen for OOM events for container "
               << containerId << ": "
               << info->oomNotifier.failure();
  }

  LOG(INFO) << "Started listening for OOM events for container "
            << containerId;

  info->oomNotifier.onReady(defer(
      PID<CgroupsMemIsolatorProcess>(this),
      &CgroupsMemIsolatorProcess::oomWaited,
      containerId,
      lambda::_1));
}


void CgroupsMemIsolatorProcess::oomWaited(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  if (future.isDiscarded()) {
    LOG(INFO) << "Discarded OOM notifier for container "
              << containerId;
  } else if (future.isFailed()) {
    LOG(ERROR) << "Listening on OOM events failed for container "
               << containerId << ": " << future.failure();
  } else {
    // Out-of-memory event happened, call the handler.
    LOG(INFO) << "OOM notifier is triggered for container " << containerId;
    oom(containerId);
  }
}


void CgroupsMemIsolatorProcess::oom(const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    // It is likely that process exited is executed before this
    // function (e.g.  The kill and OOM events happen at the same
    // time, and the process exit event arrives first.) Therefore, we
    // should not report a fatal error here.
    LOG(INFO) << "OOM detected for an already terminated executor";
    return;
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  LOG(INFO) << "OOM detected for container " << containerId;

  // Construct a "message" string to describe why the isolator
  // destroyed the executor's cgroup (in order to assist in
  // debugging).
  ostringstream message;
  message << "Memory limit exceeded: ";

  // Output the requested memory limit.
  // NOTE: If limitSwap is (has been) used then both limit_in_bytes
  // and memsw.limit_in_bytes will always be set to the same value.
  Try<Bytes> limit = cgroups::memory::limit_in_bytes(hierarchy, info->cgroup);

  if (limit.isError()) {
    LOG(ERROR) << "Failed to read 'memory.limit_in_bytes': "
               << limit.error();
  } else {
    message << "Requested: " << limit.get() << " ";
  }

  // Output the maximum memory usage.
  Try<Bytes> usage = cgroups::memory::max_usage_in_bytes(
      hierarchy, info->cgroup);

  if (usage.isError()) {
    LOG(ERROR) << "Failed to read 'memory.max_usage_in_bytes': "
               << usage.error();
  } else {
    message << "Maximum Used: " << usage.get() << "\n";
  }

  // Output 'memory.stat' of the cgroup to help with debugging.
  // NOTE: With Kernel OOM-killer enabled these stats may not reflect
  // memory state at time of OOM.
  Try<string> read = cgroups::read(hierarchy, info->cgroup, "memory.stat");
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
      stringify(usage.isSome() ? usage.get().megabytes() : 0),
      "*").get();

  info->limitation.set(
      protobuf::slave::createContainerLimitation(
          mem,
          message.str(),
          TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY));
}


void CgroupsMemIsolatorProcess::pressureListen(
    const ContainerID& containerId)
{
  CHECK(infos.contains(containerId));
  Info* info = CHECK_NOTNULL(infos[containerId]);

  foreach (Level level, levels()) {
    Try<Owned<Counter>> counter = Counter::create(
        hierarchy,
        info->cgroup,
        level);

    if (counter.isError()) {
      LOG(ERROR) << "Failed to listen on " << level << " memory pressure "
                 << "events for container " << containerId << ": "
                 << counter.error();
    } else {
      info->pressureCounters[level] = counter.get();

      LOG(INFO) << "Started listening on " << level << " memory pressure "
                << "events for container " << containerId;
    }
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
