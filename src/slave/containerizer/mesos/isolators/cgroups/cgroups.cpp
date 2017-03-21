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

#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/cgroups.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/cgroups.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

CgroupsIsolatorProcess::CgroupsIsolatorProcess(
    const Flags& _flags,
    const hashmap<string, string>& _hierarchies,
    const multihashmap<string, Owned<Subsystem>>& _subsystems)
  : ProcessBase(process::ID::generate("cgroups-isolator")),
    flags(_flags),
    hierarchies(_hierarchies),
    subsystems(_subsystems) {}


CgroupsIsolatorProcess::~CgroupsIsolatorProcess() {}


Try<Isolator*> CgroupsIsolatorProcess::create(const Flags& flags)
{
  // Subsystem name -> hierarchy path.
  hashmap<string, string> hierarchies;

  // Hierarchy path -> subsystem object.
  multihashmap<string, Owned<Subsystem>> subsystems;

  // Multimap: isolator name -> subsystem name.
  multihashmap<string, string> isolatorMap = {
    {"blkio", CGROUP_SUBSYSTEM_BLKIO_NAME},
    {"cpu", CGROUP_SUBSYSTEM_CPU_NAME},
    {"cpu", CGROUP_SUBSYSTEM_CPUACCT_NAME},
    {"cpuset", CGROUP_SUBSYSTEM_CPUSET_NAME},
    {"devices", CGROUP_SUBSYSTEM_DEVICES_NAME},
    {"hugetlb", CGROUP_SUBSYSTEM_HUGETLB_NAME},
    {"mem", CGROUP_SUBSYSTEM_MEMORY_NAME},
    {"net_cls", CGROUP_SUBSYSTEM_NET_CLS_NAME},
    {"net_prio", CGROUP_SUBSYSTEM_NET_PRIO_NAME},
    {"perf_event", CGROUP_SUBSYSTEM_PERF_EVENT_NAME},
    {"pids", CGROUP_SUBSYSTEM_PIDS_NAME},
  };

  foreach (string isolator, strings::tokenize(flags.isolation, ",")) {
    if (!strings::startsWith(isolator, "cgroups/")) {
      // Skip when the isolator is not related to cgroups.
      continue;
    }

    isolator = strings::remove(isolator, "cgroups/", strings::Mode::PREFIX);

    if (!isolatorMap.contains(isolator)) {
      return Error(
          "Unknown or unsupported isolator 'cgroups/" + isolator + "'");
    }

    // A cgroups isolator name may map to multiple subsystems. We need to
    // convert the isolator name to its associated subsystems.
    foreach (const string& subsystemName, isolatorMap.get(isolator)) {
      if (hierarchies.contains(subsystemName)) {
        // Skip when the subsystem exists.
        continue;
      }

      // Prepare hierarchy if it does not exist.
      Try<string> hierarchy = cgroups::prepare(
          flags.cgroups_hierarchy,
          subsystemName,
          flags.cgroups_root);

      if (hierarchy.isError()) {
        return Error(
            "Failed to prepare hierarchy for the subsystem '" + subsystemName +
            "': " + hierarchy.error());
      }

      // Create and load the subsystem.
      Try<Owned<Subsystem>> subsystem =
        Subsystem::create(flags, subsystemName, hierarchy.get());

      if (subsystem.isError()) {
        return Error(
            "Failed to create subsystem '" + subsystemName + "': " +
            subsystem.error());
      }

      subsystems.put(hierarchy.get(), subsystem.get());
      hierarchies.put(subsystemName, hierarchy.get());
    }
  }

  Owned<MesosIsolatorProcess> process(
      new CgroupsIsolatorProcess(flags, hierarchies, subsystems));

  return new MesosIsolator(process);
}


bool CgroupsIsolatorProcess::supportsNesting()
{
  return true;
}


void CgroupsIsolatorProcess::initialize()
{
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    spawn(subsystem.get());
  }
}


void CgroupsIsolatorProcess::finalize()
{
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    terminate(subsystem.get());
    wait(subsystem.get());
  }
}


Future<Nothing> CgroupsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Recover active containers first.
  list<Future<Nothing>> recovers;
  foreach (const ContainerState& state, states) {
    // If we are a nested container, we do not need to recover
    // anything since only top-level containers will have cgroups
    // created for them.
    if (state.container_id().has_parent()) {
      continue;
    }

    recovers.push_back(___recover(state.container_id()));
  }

  return await(recovers)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_recover,
        orphans,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::_recover(
    const hashset<ContainerID>& orphans,
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to recover active containers: " +
        strings::join(";", errors));
  }

  hashset<ContainerID> knownOrphans;
  hashset<ContainerID> unknownOrphans;

  foreach (const string& hierarchy, subsystems.keys()) {
    // TODO(jieyu): Use non-recursive version of `cgroups::get`.
    Try<vector<string>> cgroups = cgroups::get(
        hierarchy,
        flags.cgroups_root);

    if (cgroups.isError()) {
      return Failure(
          "Failed to list cgroups under '" + hierarchy + "': " +
          cgroups.error());
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

      // Skip containerId which already have been recovered.
      if (infos.contains(containerId)) {
        continue;
      }

      if (orphans.contains(containerId)) {
        knownOrphans.insert(containerId);
      } else {
        unknownOrphans.insert(containerId);
      }
    }
  }

  list<Future<Nothing>> recovers;

  foreach (const ContainerID& containerId, knownOrphans) {
    recovers.push_back(___recover(containerId));
  }

  foreach (const ContainerID& containerId, unknownOrphans) {
    recovers.push_back(___recover(containerId));
  }

  return await(recovers)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::__recover,
        unknownOrphans,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::__recover(
    const hashset<ContainerID>& unknownOrphans,
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to recover orphan containers: " +
        strings::join(";", errors));
  }

  // Known orphan cgroups will be destroyed by the containerizer using
  // the normal cleanup path. See MESOS-2367 for details.
  foreach (const ContainerID& containerId, unknownOrphans) {
    LOG(INFO) << "Cleaning up unknown orphaned container " << containerId;
    cleanup(containerId);
  }

  return Nothing();
}


Future<Nothing> CgroupsIsolatorProcess::___recover(
    const ContainerID& containerId)
{
  const string cgroup = path::join(flags.cgroups_root, containerId.value());

  list<Future<Nothing>> recovers;
  hashset<string> recoveredSubsystems;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      return Failure(
          "Failed to check the existence of the cgroup "
          "'" + cgroup + "' in hierarchy '" + hierarchy + "' "
          "for container " + stringify(containerId) +
          ": " + exists.error());
    }

    if (!exists.get()) {
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the agent dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      LOG(WARNING) << "Couldn't find the cgroup '" << cgroup << "' "
                   << "in hierarchy '" << hierarchy << "' "
                   << "for container " << containerId;

      continue;
    }

    foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
      recoveredSubsystems.insert(subsystem->name());
      recovers.push_back(subsystem->recover(containerId, cgroup));
    }
  }

  return await(recovers)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::____recover,
        containerId,
        recoveredSubsystems,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::____recover(
    const ContainerID& containerId,
    const hashset<string>& recoveredSubsystems,
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to recover subsystems: " +
        strings::join(";", errors));
  }

  CHECK(!infos.contains(containerId));

  infos[containerId] = Owned<Info>(new Info(
      containerId,
      path::join(flags.cgroups_root, containerId.value())));

  infos[containerId]->subsystems = recoveredSubsystems;

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // If we are a nested container, we do not need to prepare
  // anything since only top-level containers should have cgroups
  // created for them.
  if (containerId.has_parent()) {
    return None();
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // We save 'Info' into 'infos' first so that even if 'prepare'
  // fails, we can properly cleanup the *side effects* created below.
  infos[containerId] = Owned<Info>(new Info(
      containerId,
      path::join(flags.cgroups_root, containerId.value())));

  list<Future<Nothing>> prepares;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    string path = path::join(hierarchy, infos[containerId]->cgroup);

    VLOG(1) << "Creating cgroup at '" << path << "' "
            << "for container " << containerId;

    Try<bool> exists = cgroups::exists(
        hierarchy,
        infos[containerId]->cgroup);

    if (exists.isError()) {
      return Failure(
          "Failed to check the existence of cgroup at "
          "'" + path + "': " + exists.error());
    } else if (exists.get()) {
      return Failure("The cgroup at '" + path + "' already exists");
    }

    Try<Nothing> create = cgroups::create(
        hierarchy,
        infos[containerId]->cgroup);

    if (create.isError()) {
      return Failure(
          "Failed to create the cgroup at "
          "'" + path + "': " + create.error());
    }

    foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
      infos[containerId]->subsystems.insert(subsystem->name());
      prepares.push_back(subsystem->prepare(
          containerId,
          infos[containerId]->cgroup));
    }

    // Chown the cgroup so the executor can create nested cgroups. Do
    // not recurse so the control files are still owned by the slave
    // user and thus cannot be changed by the executor.
    //
    // TODO(haosdent): Multiple tasks under the same user can change
    // cgroups settings for each other. A better solution is using
    // cgroups namespaces and user namespaces to achieve the goal.
    //
    // NOTE: We only need to handle the case where 'flags.switch_user'
    // is true (i.e., 'containerConfig.has_user() == true'). If
    // 'flags.switch_user' is false, the cgroup will be owned by root
    // anyway since cgroups isolator requires root permission.
    if (containerConfig.has_user()) {
      Option<string> user;
      if (containerConfig.has_task_info() && containerConfig.has_rootfs()) {
        // Command task that has a rootfs. In this case, the executor
        // will be running under root, and the command task itself
        // might be running under a different user.
        //
        // TODO(jieyu): The caveat here is that if the 'user' in
        // task's command is not set, we don't know exactly what user
        // the task will be running as because we don't know the
        // framework user. We do not support this case right now.
        if (containerConfig.task_info().command().has_user()) {
          user = containerConfig.task_info().command().user();
        }
      } else {
        user = containerConfig.user();
      }

      if (user.isSome()) {
        VLOG(1) << "Chown the cgroup at '" << path << "' to user "
                << "'" << user.get() << "' for container " << containerId;

        Try<Nothing> chown = os::chown(
            user.get(),
            path,
            false);

        if (chown.isError()) {
          return Failure(
              "Failed to chown the cgroup at '" + path + "' "
              "to user '" + user.get() + "': " + chown.error());
        }
      }
    }
  }

  return await(prepares)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_prepare,
        containerId,
        containerConfig,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to prepare subsystems: " +
        strings::join(";", errors));
  }

  // TODO(haosdent): Here we assume the command executor's resources
  // include the task's resources. Revisit here if this semantics
  // changes.
  return update(containerId, containerConfig.executor_info().resources())
    .then([]() { return Option<ContainerLaunchInfo>::none(); });
}


Future<Nothing> CgroupsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  // If we are a nested container, we inherit
  // the cgroup from our root ancestor.
  ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  if (!infos.contains(rootContainerId)) {
    return Failure("Failed to isolate the container: Unknown root container");
  }

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    Try<Nothing> assign = cgroups::assign(
        hierarchy,
        infos[rootContainerId]->cgroup,
        pid);

    if (assign.isError()) {
      string message =
        "Failed to assign pid " + stringify(pid) + " to cgroup at "
        "'" + path::join(hierarchy, infos[rootContainerId]->cgroup) + "'"
        ": " + assign.error();

      LOG(ERROR) << message;

      return Failure(message);
    }
  }

  // We currently can't call `subsystem->isolate()` on nested
  // containers, because we don't call `prepare()`, `recover()`, or
  // `cleanup()` on them either. If we were to call `isolate()` on
  // them, the call would likely fail because the subsystem doesn't
  // know about the container. This is currently OK because the only
  // cgroup isolator that even implements `isolate()` is the
  // `NetClsSubsystem` and it doesn't do anything with the `pid`
  // passed in.
  //
  // TODO(klueska): In the future we should revisit this to make
  // sure that doing things this way is sufficient (or otherwise
  // update our invariants to allow us to call this here).
  if (containerId.has_parent()) {
    return Nothing();
  }

  list<Future<Nothing>> isolates;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    isolates.push_back(subsystem->isolate(
        containerId,
        infos[containerId]->cgroup,
        pid));
  }

  return await(isolates)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_isolate,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::_isolate(
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to isolate subsystems: " +
        strings::join(";", errors));
  }

  return Nothing();
}


Future<ContainerLimitation> CgroupsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // Since we do not maintain cgroups for nested containers
  // directly, we simply return a pending future here, indicating
  // that the limit for the nested container will never be reached.
  if (containerId.has_parent()) {
    return Future<ContainerLimitation>();
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      subsystem->watch(containerId, infos[containerId]->cgroup)
        .onAny(defer(
            PID<CgroupsIsolatorProcess>(this),
            &CgroupsIsolatorProcess::_watch,
            containerId,
            lambda::_1));
    }
  }

  return infos[containerId]->limitation.future();
}


void CgroupsIsolatorProcess::_watch(
    const ContainerID& containerId,
    const Future<ContainerLimitation>& future)
{
  if (!infos.contains(containerId)) {
    return;
  }

  CHECK(!future.isPending());

  infos[containerId]->limitation.set(future);
}


Future<Nothing> CgroupsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<Nothing>> updates;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      updates.push_back(subsystem->update(
          containerId,
          infos[containerId]->cgroup,
          resources));
    }
  }

  return await(updates)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_update,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::_update(
    const list<Future<Nothing>>& futures)
{
  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to update subsystems: " +
        strings::join(";", errors));
  }

  return Nothing();
}


Future<ResourceStatistics> CgroupsIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<ResourceStatistics>> usages;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      usages.push_back(subsystem->usage(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(usages)
    .then([containerId](const list<Future<ResourceStatistics>>& _usages) {
      ResourceStatistics result;

      foreach (const Future<ResourceStatistics>& statistics, _usages) {
        if (statistics.isReady()) {
          result.MergeFrom(statistics.get());
        } else {
          LOG(WARNING) << "Skipping resource statistic for container "
                       << containerId << " because: "
                       << (statistics.isFailed() ? statistics.failure()
                                                 : "discarded");
        }
      }

      return result;
    });
}


Future<ContainerStatus> CgroupsIsolatorProcess::status(
    const ContainerID& containerId)
{
  // TODO(jieyu): Currently, all nested containers share the same
  // cgroup as their parent container. Revisit this once this is no
  // long true.
  if (containerId.has_parent()) {
    return status(containerId.parent());
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<ContainerStatus>> statuses;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      statuses.push_back(subsystem->status(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(statuses)
    .then([containerId](const list<Future<ContainerStatus>>& _statuses) {
      ContainerStatus result;

      foreach (const Future<ContainerStatus>& status, _statuses) {
        if (status.isReady()) {
          result.MergeFrom(status.get());
        } else {
          LOG(WARNING) << "Skipping status for container " << containerId
                       << " because: "
                       << (status.isFailed() ? status.failure() : "discarded");
        }
      }

      return result;
    });
}


Future<Nothing> CgroupsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // If we are a nested container, we do not need to clean anything up
  // since only top-level containers should have cgroups created for them.
  if (containerId.has_parent()) {
    return Nothing();
  }

  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  list<Future<Nothing>> cleanups;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      cleanups.push_back(subsystem->cleanup(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(cleanups)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const list<Future<Nothing>>& futures)
{
  CHECK(infos.contains(containerId));

  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to cleanup subsystems: " +
        strings::join(";", errors));
  }

  list<Future<Nothing>> destroys;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
      if (infos[containerId]->subsystems.contains(subsystem->name())) {
        destroys.push_back(cgroups::destroy(
            hierarchy,
            infos[containerId]->cgroup,
            cgroups::DESTROY_TIMEOUT));

        break;
      }
    }
  }

  return await(destroys)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::__cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::__cleanup(
    const ContainerID& containerId,
    const list<Future<Nothing>>& futures)
{
  CHECK(infos.contains(containerId));

  vector<string> errors;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (errors.size() > 0) {
    return Failure(
        "Failed to destroy cgroups: " +
        strings::join(";", errors));
  }

  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
