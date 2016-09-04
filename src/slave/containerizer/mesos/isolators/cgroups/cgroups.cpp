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
    {"cpu", CGROUP_SUBSYSTEM_CPU_NAME},
    {"cpu", CGROUP_SUBSYSTEM_CPUACCT_NAME},
    {"devices", CGROUP_SUBSYSTEM_DEVICES_NAME},
    {"mem", CGROUP_SUBSYSTEM_MEMORY_NAME},
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
      recovers.push_back(subsystem->recover(containerId));
    }
  }

  return await(recovers)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::____recover,
        containerId,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::____recover(
    const ContainerID& containerId,
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

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // We save 'Info' into 'infos' first so that even if 'prepare'
  // fails, we can properly cleanup the *side effects* created below.
  infos[containerId] = Owned<Info>(new Info(
      containerId,
      path::join(flags.cgroups_root, containerId.value())));

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

    // Chown the cgroup so the executor can create nested cgroups. Do
    // not recurse so the control files are still owned by the slave
    // user and thus cannot be changed by the executor.
    //
    // TODO(haosdent): Multiple tasks under the same user can change
    // cgroups settings for each other. A better solution is using
    // cgroups namespaces and user namespaces to achieve the goal.
    if (containerConfig.has_user()) {
      Try<Nothing> chown = os::chown(
          containerConfig.user(),
          path,
          false);

      if (chown.isError()) {
        return Failure(
            "Failed to chown the cgroup at "
            "'" + path + "': " + chown.error());
      }
    }
  }

  list<Future<Nothing>> prepares;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    prepares.push_back(subsystem->prepare(containerId));
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
  if (!infos.contains(containerId)) {
    return Failure("Failed to isolate the container: Unknown container");
  }

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    Try<Nothing> assign = cgroups::assign(
        hierarchy,
        infos[containerId]->cgroup,
        pid);

    if (assign.isError()) {
      string message =
        "Failed to assign pid " + stringify(pid) + " to cgroup at "
        "'" + path::join(hierarchy, infos[containerId]->cgroup) + "'"
        ": " + assign.error();

      LOG(ERROR) << message;

      return Failure(message);
    }
  }

  list<Future<Nothing>> isolates;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    isolates.push_back(subsystem->isolate(containerId, pid));
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
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    subsystem->watch(containerId)
      .onAny(defer(
          PID<CgroupsIsolatorProcess>(this),
          &CgroupsIsolatorProcess::_watch,
          containerId,
          lambda::_1));
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
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<Nothing>> updates;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    updates.push_back(subsystem->update(containerId, resources));
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
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<ResourceStatistics>> usages;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    usages.push_back(subsystem->usage(containerId));
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
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  list<Future<ContainerStatus>> statuses;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    statuses.push_back(subsystem->status(containerId));
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
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  list<Future<Nothing>> cleanups;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    cleanups.push_back(subsystem->cleanup(containerId));
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
    destroys.push_back(cgroups::destroy(
        hierarchy,
        infos[containerId]->cgroup,
        cgroups::DESTROY_TIMEOUT));
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
