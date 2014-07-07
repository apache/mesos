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

#include <mesos/values.hpp>
#include <mesos/resources.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/pid.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// CPU subsystem constants.
const uint64_t CPU_SHARES_PER_CPU = 1024;
const uint64_t MIN_CPU_SHARES = 10;
const Duration CPU_CFS_PERIOD = Milliseconds(100); // Linux default.
const Duration MIN_CPU_CFS_QUOTA = Milliseconds(1);


template<class T>
static Future<Option<T> > none() { return None(); }

CgroupsCpushareIsolatorProcess::CgroupsCpushareIsolatorProcess(
    const Flags& _flags,
    const hashmap<string, string>& _hierarchies)
  : flags(_flags), hierarchies(_hierarchies) {}


CgroupsCpushareIsolatorProcess::~CgroupsCpushareIsolatorProcess() {}


Try<Isolator*> CgroupsCpushareIsolatorProcess::create(
    const Flags& flags)
{
  hashmap<string, string> hierarchies;

  vector<string> subsystems;
  subsystems.push_back("cpu");
  subsystems.push_back("cpuacct");

  foreach (const string& subsystem, subsystems) {
    Try<string> hierarchy = cgroups::prepare(
        flags.cgroups_hierarchy, subsystem, flags.cgroups_root);

    if (hierarchy.isError()) {
      return Error("Failed to create isolator: " + hierarchy.error());
    }

    hierarchies[subsystem] = hierarchy.get();
  }

  if (flags.cgroups_enable_cfs) {
    Try<bool> exists = cgroups::exists(
        hierarchies["cpu"], flags.cgroups_root, "cpu.cfs_quota_us");

    if (exists.isError() || !exists.get()) {
      return Error("Failed to find 'cpu.cfs_quota_us'. Your kernel "
                   "might be too old to use the CFS cgroups feature.");
    }
  }

  process::Owned<IsolatorProcess> process(
      new CgroupsCpushareIsolatorProcess(flags, hierarchies));

  return new Isolator(process);
}


Future<Nothing> CgroupsCpushareIsolatorProcess::recover(
    const list<state::RunState>& states)
{
  hashset<string> cgroups;

  foreach (const state::RunState& state, states) {
    if (!state.id.isSome()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("ContainerID is required to recover");
    }

    const ContainerID& containerId = state.id.get();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchies["cpu"], cgroup);
    if (exists.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("Failed to check cgroup for container '" +
                     stringify(containerId) + "'");
    }

    if (!exists.get()) {
      // This may occur if the executor has exited and the isolator has
      // destroyed the cgroup but the slave dies before noticing this. This
      // will be detected when the containerizer tries to monitor the
      // executor's pid.
      LOG(WARNING) << "Couldn't find cgroup for container " << containerId;
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);;
    cgroups.insert(cgroup);
  }

  // Remove orphans in the cpu hierarchy.
  Try<vector<string> > orphans = cgroups::get(
      hierarchies["cpu"], flags.cgroups_root);
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
      LOG(INFO) << "Removing orphaned cgroup"
                << " '" << path::join("cpu", orphan) << "'";
      // We don't wait on the destroy as we don't want to block recovery.
      cgroups::destroy(
          hierarchies["cpu"], orphan, cgroups::DESTROY_TIMEOUT);
    }
  }

  // Remove orphans in the cpuacct hierarchy.
  orphans = cgroups::get(hierarchies["cpuacct"], flags.cgroups_root);
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
      LOG(INFO) << "Removing orphaned cgroup"
                << " '" << path::join("cpuacct", orphan) << "'";
      // We don't wait on the destroy as we don't want to block recovery.
      cgroups::destroy(
          hierarchies["cpuacct"], orphan, cgroups::DESTROY_TIMEOUT);
    }
  }

  return Nothing();
}


Future<Option<CommandInfo> > CgroupsCpushareIsolatorProcess::prepare(
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

  // Create a 'cpu' cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchies["cpu"], info->cgroup);

  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  } else if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  Try<Nothing> create = cgroups::create(hierarchies["cpu"], info->cgroup);
  if (create.isError()) {
    return Failure("Failed to prepare isolator: " + create.error());
  }

  // Create a 'cpuacct' cgroup for this container.
  exists = cgroups::exists(hierarchies["cpuacct"], info->cgroup);

  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  } else if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  create = cgroups::create(hierarchies["cpuacct"], info->cgroup);
  if (create.isError()) {
    return Failure("Failed to prepare isolator: " + create.error());
  }

  return update(containerId, executorInfo.resources())
    .then(lambda::bind(none<CommandInfo>));
}


Future<Nothing> CgroupsCpushareIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  CHECK(info->pid.isNone());
  info->pid = pid;

  Try<Nothing> assign = cgroups::assign(hierarchies["cpu"], info->cgroup, pid);
  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign container '" << info->containerId
               << " to its own cgroup '"
               << path::join(hierarchies["cpu"], info->cgroup)
               << "' : " << assign.error();
    return Failure("Failed to isolate container: " + assign.error());
  }

  assign = cgroups::assign(hierarchies["cpuacct"], info->cgroup, pid);
  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign container '" << info->containerId
               << " to its own cgroup '"
               << path::join(hierarchies["cpuacct"], info->cgroup)
               << "' : " << assign.error();
    return Failure("Failed to isolate container: " + assign.error());
  }

  return Nothing();
}


Future<Limitation> CgroupsCpushareIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  CHECK_NOTNULL(infos[containerId]);

  return infos[containerId]->limitation.future();
}


Future<Nothing> CgroupsCpushareIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (resources.cpus().isNone()) {
    return Failure("No cpus resource given");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Option<string>& hierarchy = hierarchies.get("cpu");

  if (hierarchy.isNone()) {
    return Failure("No 'cpu' hierarchy");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  double cpus = resources.cpus().get();

  // Always set cpu.shares.
  uint64_t shares =
    std::max((uint64_t) (CPU_SHARES_PER_CPU * cpus), MIN_CPU_SHARES);

  Try<Nothing> write = cgroups::cpu::shares(
      hierarchy.get(), info->cgroup, shares);

  if (write.isError()) {
    return Failure("Failed to update 'cpu.shares': " + write.error());
  }

  LOG(INFO) << "Updated 'cpu.shares' to " << shares
            << " (cpus " << cpus << ")"
            << " for container " << containerId;

  // Set cfs quota if enabled.
  if (flags.cgroups_enable_cfs) {
    write = cgroups::cpu::cfs_period_us(
        hierarchy.get(), info->cgroup, CPU_CFS_PERIOD);

    if (write.isError()) {
      return Failure("Failed to update 'cpu.cfs_period_us': " + write.error());
    }

    Duration quota = std::max(CPU_CFS_PERIOD * cpus, MIN_CPU_CFS_QUOTA);

    write = cgroups::cpu::cfs_quota_us(hierarchy.get(), info->cgroup, quota);

    if (write.isError()) {
      return Failure("Failed to update 'cpu.cfs_quota_us': " + write.error());
    }

    LOG(INFO) << "Updated 'cpu.cfs_period_us' to " << CPU_CFS_PERIOD
              << " and 'cpu.cfs_quota_us' to " << quota
              << " (cpus " << cpus << ")"
              << " for container " << containerId;
  }

  return Nothing();
}


Future<ResourceStatistics> CgroupsCpushareIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  ResourceStatistics result;

  // Get the number of clock ticks, used for cpu accounting.
  static long ticks = sysconf(_SC_CLK_TCK);

  PCHECK(ticks > 0) << "Failed to get sysconf(_SC_CLK_TCK)";

  // Add the cpuacct.stat information.
  Try<hashmap<string, uint64_t> > stat =
    cgroups::stat(hierarchies["cpuacct"], info->cgroup, "cpuacct.stat");

  if (stat.isError()) {
    return Failure("Failed to read cpuacct.stat: " + stat.error());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g., cgroups::cpuacct::stat.
  Option<uint64_t> user = stat.get().get("user");
  Option<uint64_t> system = stat.get().get("system");

  if (user.isSome() && system.isSome()) {
    result.set_cpus_user_time_secs((double) user.get() / (double) ticks);
    result.set_cpus_system_time_secs((double) system.get() / (double) ticks);
  }

  // Add the cpu.stat information only if CFS is enabled.
  if (flags.cgroups_enable_cfs) {
    stat = cgroups::stat(hierarchies["cpu"], info->cgroup, "cpu.stat");

    if (stat.isError()) {
      return Failure("Failed to read cpu.stat: " + stat.error());
    }

    Option<uint64_t> nr_periods = stat.get().get("nr_periods");
    if (nr_periods.isSome()) {
      result.set_cpus_nr_periods(nr_periods.get());
    }

    Option<uint64_t> nr_throttled = stat.get().get("nr_throttled");
    if (nr_throttled.isSome()) {
      result.set_cpus_nr_throttled(nr_throttled.get());
    }

    Option<uint64_t> throttled_time = stat.get().get("throttled_time");
    if (throttled_time.isSome()) {
      result.set_cpus_throttled_time_secs(
          Nanoseconds(throttled_time.get()).secs());
    }
  }

  return result;
}


namespace {

Future<Nothing> _nothing() { return Nothing(); }

} // namespace {


Future<Nothing> CgroupsCpushareIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;
    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  list<Future<Nothing> > futures;
  foreachvalue (const string& hierarchy, hierarchies) {
    futures.push_back(
        cgroups::destroy(hierarchy, info->cgroup, cgroups::DESTROY_TIMEOUT));
  }

  return collect(futures)
    .onAny(defer(PID<CgroupsCpushareIsolatorProcess>(this),
                &CgroupsCpushareIsolatorProcess::_cleanup,
                containerId,
                lambda::_1))
    .then(lambda::bind(&_nothing));
}


Future<list<Nothing> > CgroupsCpushareIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const Future<list<Nothing> >& future)
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

  return future;
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
