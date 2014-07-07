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

#include <vector>

#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/pid.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

#include "linux/cgroups.hpp"

#include "slave/containerizer/isolators/cgroups/mem.hpp"

using namespace process;

using std::list;
using std::ostringstream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// Memory subsystem constants.
const Bytes MIN_MEMORY = Megabytes(32);


template<class T>
static Future<Option<T> > none() { return None(); }

CgroupsMemIsolatorProcess::CgroupsMemIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags), hierarchy(_hierarchy) {}


CgroupsMemIsolatorProcess::~CgroupsMemIsolatorProcess() {}


Try<Isolator*> CgroupsMemIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy, "memory", flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create memory cgroup: " + hierarchy.error());
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

  process::Owned<IsolatorProcess> process(
      new CgroupsMemIsolatorProcess(flags, hierarchy.get()));

  return new Isolator(process);
}


Future<Nothing> CgroupsMemIsolatorProcess::recover(
    const list<state::RunState>& states)
{
  hashset<string> cgroups;

  foreach (const state::RunState& state, states) {
    if (state.id.isNone()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("ContainerID is required to recover");
    }

    const ContainerID& containerId = state.id.get();
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
      // This may occur if the executor has exited and the isolator has
      // destroyed the cgroup but the slave dies before noticing this. This
      // will be detected when the containerizer tries to monitor the
      // executor's pid.
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);
    cgroups.insert(cgroup);

    oomListen(containerId);
  }

  Try<vector<string> > orphans = cgroups::get(
      hierarchy, flags.cgroups_root);
  if (orphans.isError()) {
    foreachvalue (Info* info, infos) {
      delete info;
    }
    infos.clear();
    return Failure(orphans.error());
  }

  foreach (const string& orphan, orphans.get()) {
    // Ignore the slave cgroup (see the --slave_subsystems flag).
    // TODO(idownes): Remove this when the cgroups layout is updated,
    // see MESOS-1185.
    if (orphan == path::join(flags.cgroups_root, "slave")) {
      continue;
    }

    if (!cgroups.contains(orphan)) {
      LOG(INFO) << "Removing orphaned cgroup '" << orphan << "'";
      // We don't wait on the destroy as we don't want to block recovery.
      cgroups::destroy(hierarchy, orphan, cgroups::DESTROY_TIMEOUT);
    }
  }

  return Nothing();
}


Future<Option<CommandInfo> > CgroupsMemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // TODO(bmahler): Don't insert into 'infos' unless we create the
  // cgroup successfully. It's safe for now because 'cleanup' gets
  // called if we return a Failure, but cleanup will fail because
  // the cgroup does not exist when cgroups::destroy is called.
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

  oomListen(containerId);

  return update(containerId, executorInfo.resources())
    .then(lambda::bind(none<CommandInfo>));
}


Future<Nothing> CgroupsMemIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  CHECK(info->pid.isNone());
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


Future<Limitation> CgroupsMemIsolatorProcess::watch(
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
    return Failure("Failed to set 'memory.soft_limit_in_bytes': "
        + write.error());
  }

  LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to " << limit
            << " for container " << containerId;

  // Read the existing limit.
  Try<Bytes> currentLimit =
    cgroups::memory::limit_in_bytes(hierarchy, info->cgroup);

  if (currentLimit.isError()) {
    return Failure(
        "Failed to read 'memory.limit_in_bytes': " + currentLimit.error());
  }

  // Determine whether to set the hard limit. If this is the first
  // time (info->pid.isNone()), or we're raising the existing limit,
  // then we can update the hard limit safely. Otherwise, if we need
  // to decrease 'memory.limit_in_bytes' we may induce an OOM if too
  // much memory is in use. As a result, we only update the soft
  // limit when the memory reservation is being reduced. This is
  // probably okay if the machine has available resources.
  // TODO(benh): Introduce a MemoryWatcherProcess which monitors the
  // discrepancy between usage and soft limit and introduces a "manual oom" if
  // necessary.
  if (info->pid.isNone() || limit > currentLimit.get()) {
    write = cgroups::memory::limit_in_bytes(hierarchy, info->cgroup, limit);

    if (write.isError()) {
      return Failure("Failed to set 'memory.limit_in_bytes': " +
                     write.error());
    }

    LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << limit
              << " for container " << containerId;
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

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g, cgroups::memory::stat.
  result.set_mem_rss_bytes(usage.get().bytes());

  Try<hashmap<string, uint64_t> > stat =
    cgroups::stat(hierarchy, info->cgroup, "memory.stat");

  if (stat.isError()) {
    return Failure("Failed to read memory.stat: " + stat.error());
  }

  Option<uint64_t> total_cache = stat.get().get("total_cache");
  if (total_cache.isSome()) {
    result.set_mem_file_bytes(total_cache.get());
  }

  Option<uint64_t> total_rss = stat.get().get("total_rss");
  if (total_rss.isSome()) {
    result.set_mem_anon_bytes(total_rss.get());
  }

  Option<uint64_t> total_mapped_file = stat.get().get("total_mapped_file");
  if (total_mapped_file.isSome()) {
    result.set_mem_mapped_file_bytes(total_mapped_file.get());
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
  Try<Bytes> limit = cgroups::memory::limit_in_bytes(hierarchy, info->cgroup);

  if (limit.isError()) {
    LOG(ERROR) << "Failed to read 'memory.limit_in_bytes': " << limit.error();
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

  Resource mem = Resources::parse(
      "mem",
      stringify(usage.isSome() ? usage.get().bytes() : 0),
      "*").get();

  info->limitation.set(Limitation(mem, message.str()));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
