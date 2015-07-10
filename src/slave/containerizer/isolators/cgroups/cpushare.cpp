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
#include <mesos/type_utils.hpp>
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
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"
#include "linux/sched.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"

using namespace process;

using std::list;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ExecutorRunState;
using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;
using mesos::slave::Limitation;

namespace mesos {
namespace internal {
namespace slave {

CgroupsCpushareIsolatorProcess::CgroupsCpushareIsolatorProcess(
    const Flags& _flags,
    const hashmap<string, string>& _hierarchies,
    const vector<string>& _subsystems)
  : flags(_flags),
    hierarchies(_hierarchies),
    subsystems(_subsystems) {}


CgroupsCpushareIsolatorProcess::~CgroupsCpushareIsolatorProcess() {}


Try<Isolator*> CgroupsCpushareIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchyCpu = cgroups::prepare(
        flags.cgroups_hierarchy,
        "cpu",
        flags.cgroups_root);

  if (hierarchyCpu.isError()) {
    return Error(
        "Failed to prepare hierarchy for cpu subsystem: " +
        hierarchyCpu.error());
  }

  Try<string> hierarchyCpuacct = cgroups::prepare(
        flags.cgroups_hierarchy,
        "cpuacct",
        flags.cgroups_root);

  if (hierarchyCpuacct.isError()) {
    return Error(
        "Failed to prepare hierarchy for cpuacct subsystem: " +
        hierarchyCpuacct.error());
  }

  hashmap<string, string> hierarchies;
  vector<string> subsystems;

  hierarchies["cpu"] = hierarchyCpu.get();
  hierarchies["cpuacct"] = hierarchyCpuacct.get();

  if (hierarchyCpu.get() == hierarchyCpuacct.get()) {
    // Subsystem cpu and cpuacct are co-mounted (e.g., systemd).
    hierarchies["cpu,cpuacct"] = hierarchyCpu.get();
    subsystems.push_back("cpu,cpuacct");

    // Ensure that no other subsystem is attached to the hierarchy.
    Try<set<string>> _subsystems = cgroups::subsystems(hierarchyCpu.get());
    if (_subsystems.isError()) {
      return Error(
          "Failed to get the list of attached subsystems for hierarchy " +
          hierarchyCpu.get());
    } else if (_subsystems.get().size() != 2) {
      return Error(
          "Unexpected subsystems found attached to the hierarchy " +
          hierarchyCpu.get());
    }
  } else {
    // Subsystem cpu and cpuacct are mounted separately.
    subsystems.push_back("cpu");
    subsystems.push_back("cpuacct");

    // Ensure that no other subsystem is attached to each of the
    // hierarchy.
    Try<set<string>> _subsystems = cgroups::subsystems(hierarchyCpu.get());
    if (_subsystems.isError()) {
      return Error(
          "Failed to get the list of attached subsystems for hierarchy " +
          hierarchyCpu.get());
    } else if (_subsystems.get().size() != 1) {
      return Error(
          "Unexpected subsystems found attached to the hierarchy " +
          hierarchyCpu.get());
    }

    _subsystems = cgroups::subsystems(hierarchyCpuacct.get());
    if (_subsystems.isError()) {
      return Error(
          "Failed to get the list of attached subsystems for hierarchy " +
          hierarchyCpuacct.get());
    } else if (_subsystems.get().size() != 1) {
      return Error(
          "Unexpected subsystems found attached to the hierarchy " +
          hierarchyCpuacct.get());
    }
  }

  if (flags.cgroups_enable_cfs) {
    Try<bool> exists = cgroups::exists(
        hierarchies["cpu"],
        flags.cgroups_root,
        "cpu.cfs_quota_us");

    if (exists.isError() || !exists.get()) {
      return Error(
          "Failed to find 'cpu.cfs_quota_us'. Your kernel "
          "might be too old to use the CFS cgroups feature.");
    }
  }

  process::Owned<IsolatorProcess> process(
      new CgroupsCpushareIsolatorProcess(flags, hierarchies, subsystems));

  return new Isolator(process);
}


Future<Nothing> CgroupsCpushareIsolatorProcess::recover(
    const list<ExecutorRunState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ExecutorRunState& state, states) {
    const ContainerID& containerId = state.id;
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchies["cpu"], cgroup);
    if (exists.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure(
          "Failed to check cgroup for container " + stringify(containerId));
    }

    if (!exists.get()) {
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      LOG(WARNING) << "Couldn't find cgroup for container " << containerId;
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);
  }

  // Remove orphan cgroups.
  foreach (const string& subsystem, subsystems) {
    Try<vector<string>> cgroups = cgroups::get(
        hierarchies[subsystem],
        flags.cgroups_root);

    if (cgroups.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure(cgroups.error());
    }

    foreach (const string& cgroup, cgroups.get()) {
      // Ignore the slave cgroup (see the --slave_subsystems flag).
      // TODO(idownes): Remove this when the cgroups layout is
      // updated, see MESOS-1185.
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

      LOG(INFO) << "Removing unknown orphaned cgroup '"
                << path::join(subsystem, cgroup) << "'";

      // We don't wait on the destroy as we don't want to block recovery.
      cgroups::destroy(
          hierarchies[subsystem],
          cgroup,
          cgroups::DESTROY_TIMEOUT);
    }
  }

  return Nothing();
}


Future<Option<CommandInfo>> CgroupsCpushareIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& rootfs,
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

  foreach (const string& subsystem, subsystems) {
    Try<bool> exists = cgroups::exists(hierarchies[subsystem], info->cgroup);
    if (exists.isError()) {
      return Failure("Failed to prepare isolator: " + exists.error());
    } else if (exists.get()) {
      return Failure("Failed to prepare isolator: cgroup already exists");
    }

    Try<Nothing> create = cgroups::create(hierarchies[subsystem], info->cgroup);
    if (create.isError()) {
      return Failure("Failed to prepare isolator: " + create.error());
    }

    // Chown the cgroup so the executor can create nested cgroups. Do
    // not recurse so the control files are still owned by the slave
    // user and thus cannot be changed by the executor.
    if (user.isSome()) {
      Try<Nothing> chown = os::chown(
          user.get(),
          path::join(hierarchies[subsystem], info->cgroup),
          false);
      if (chown.isError()) {
        return Failure("Failed to prepare isolator: " + chown.error());
      }
    }
  }

  return update(containerId, executorInfo.resources())
    .then([]() -> Future<Option<CommandInfo>> {
      return None();
    });
}


Future<Nothing> CgroupsCpushareIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  CHECK_NONE(info->pid);
  info->pid = pid;

  foreach (const string& subsystem, subsystems) {
    Try<Nothing> assign = cgroups::assign(
        hierarchies[subsystem],
        info->cgroup,
        pid);

    if (assign.isError()) {
      LOG(ERROR) << "Failed to assign container '" << info->containerId
                 << " to its own cgroup '"
                 << path::join(hierarchies[subsystem], info->cgroup)
                 << "' : " << assign.error();

      return Failure("Failed to isolate container: " + assign.error());
    }
  }

  // NOTE: This only sets the executor and descendants to IDLE policy
  // if the initial CPU resource is revocable and not if initial CPU
  // is non-revocable but subsequent updates include revocable CPU.
  if (info->resources.isSome() &&
      info->resources.get().revocable().cpus().isSome() &&
      flags.revocable_cpu_low_priority) {
    Try<Nothing> set = sched::policy::set(sched::Policy::IDLE, pid);
    if (set.isError()) {
      return Failure("Failed to set SCHED_IDLE for pid " + stringify(pid) +
                     " in container '" + stringify(containerId) + "'" +
                     " with revocable CPU: " + set.error());
    }

    LOG(INFO) << "Set scheduling policy to SCHED_IDLE for pid " << pid
              << " in container '" << containerId << "' because it includes '"
              << info->resources.get().revocable().cpus().get()
              << "' revocable CPU";
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
  info->resources = resources;

  double cpus = resources.cpus().get();

  // Always set cpu.shares.
  uint64_t shares;

  if (flags.revocable_cpu_low_priority &&
      resources.revocable().cpus().isSome()) {
    shares = std::max(
        (uint64_t) (CPU_SHARES_PER_CPU_REVOCABLE * cpus),
        MIN_CPU_SHARES);
  } else {
    shares = std::max(
        (uint64_t) (CPU_SHARES_PER_CPU * cpus),
        MIN_CPU_SHARES);
  }

  Try<Nothing> write = cgroups::cpu::shares(
      hierarchy.get(),
      info->cgroup,
      shares);

  if (write.isError()) {
    return Failure("Failed to update 'cpu.shares': " + write.error());
  }

  LOG(INFO) << "Updated 'cpu.shares' to " << shares
            << " (cpus " << cpus << ")"
            << " for container " << containerId;

  // Set cfs quota if enabled.
  if (flags.cgroups_enable_cfs) {
    write = cgroups::cpu::cfs_period_us(
        hierarchy.get(),
        info->cgroup,
        CPU_CFS_PERIOD);

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

  // TODO(chzhcn): Getting the number of processes and threads is
  // available as long as any cgroup subsystem is used so this best
  // not be tied to a specific cgroup isolator. A better place is
  // probably Linux Launcher, which uses the cgroup freezer subsystem.
  // That requires some change for it to adopt the new semantics of
  // reporting subsystem-independent cgroup usage.
  // Note: The complexity of this operation is linear to the number of
  // processes and threads in a container: the kernel has to allocate
  // memory to contain the list of pids or tids; the userspace has to
  // parse the cgroup files to get the size. If this proves to be a
  // performance bottleneck, some kind of rate limiting mechanism
  // needs to be employed.
  if (flags.cgroups_cpu_enable_pids_and_tids_count) {
    Try<std::set<pid_t>> pids =
      cgroups::processes(hierarchies["cpuacct"], info->cgroup);
    if (pids.isError()) {
      return Failure("Failed to get number of processes: " + pids.error());
    }

    result.set_processes(pids.get().size());

    Try<std::set<pid_t>> tids =
      cgroups::threads(hierarchies["cpuacct"], info->cgroup);
    if (tids.isError()) {
      return Failure("Failed to get number of threads: " + tids.error());
    }

    result.set_threads(tids.get().size());
  }

  // Get the number of clock ticks, used for cpu accounting.
  static long ticks = sysconf(_SC_CLK_TCK);

  PCHECK(ticks > 0) << "Failed to get sysconf(_SC_CLK_TCK)";

  // Add the cpuacct.stat information.
  Try<hashmap<string, uint64_t>> stat = cgroups::stat(
      hierarchies["cpuacct"],
      info->cgroup,
      "cpuacct.stat");

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

  list<Future<Nothing>> futures;
  foreach (const string& subsystem, subsystems) {
    futures.push_back(cgroups::destroy(
        hierarchies[subsystem],
        info->cgroup,
        cgroups::DESTROY_TIMEOUT));
  }

  return collect(futures)
    .onAny(defer(PID<CgroupsCpushareIsolatorProcess>(this),
                &CgroupsCpushareIsolatorProcess::_cleanup,
                containerId,
                lambda::_1))
    .then([]() { return Nothing(); });
}


Future<list<Nothing>> CgroupsCpushareIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const Future<list<Nothing>>& future)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  CHECK_NOTNULL(infos[containerId]);

  if (!future.isReady()) {
    return Failure(
        "Failed to clean up container " + stringify(containerId) +
        " : " + (future.isFailed() ? future.failure() : "discarded"));
  }

  delete infos[containerId];
  infos.erase(containerId);

  return future;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
