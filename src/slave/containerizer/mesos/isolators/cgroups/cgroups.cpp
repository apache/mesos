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

#include <set>
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
#include "linux/fs.hpp"
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/cgroups.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

using mesos::internal::protobuf::slave::containerSymlinkOperation;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerMountInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

CgroupsIsolatorProcess::CgroupsIsolatorProcess(
    const Flags& _flags,
    const multihashmap<string, Owned<Subsystem>>& _subsystems)
  : ProcessBase(process::ID::generate("cgroups-isolator")),
    flags(_flags),
    subsystems(_subsystems) {}


CgroupsIsolatorProcess::~CgroupsIsolatorProcess() {}


Try<Isolator*> CgroupsIsolatorProcess::create(const Flags& flags)
{
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

  // All the subsystems currently supported by Mesos.
  set<string> supportedSubsystems;
  foreachvalue (const string& subsystemName, isolatorMap) {
    supportedSubsystems.insert(subsystemName);
  }

  // The subsystems to be loaded.
  set<string> subsystemSet;

  if (strings::contains(flags.isolation, "cgroups/all")) {
    Try<set<string>> enabledSubsystems = cgroups::subsystems();
    if (enabledSubsystems.isError()) {
      return Error(
          "Failed to get the enabled subsystems: " + enabledSubsystems.error());
    }

    foreach (const string& subsystemName, enabledSubsystems.get()) {
      if (supportedSubsystems.count(subsystemName) == 0) {
        // Skip when the subsystem is not supported by Mesos yet.
        LOG(WARNING) << "Cannot automatically load the subsystem '"
                     << subsystemName << "' because it is not "
                     << "supported by Mesos cgroups isolator yet";
        continue;
      }

      subsystemSet.insert(subsystemName);
    }

    if (subsystemSet.empty()) {
      return Error("No subsystems are enabled by the kernel");
    }

    LOG(INFO) << "Automatically loading subsystems: "
              << stringify(subsystemSet);
  } else {
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
        subsystemSet.insert(subsystemName);
      }
    }
  }

  CHECK(!subsystemSet.empty());

  foreach (const string& subsystemName, subsystemSet) {
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
  }

  Owned<MesosIsolatorProcess> process(
      new CgroupsIsolatorProcess(flags, subsystems));

  return new MesosIsolator(process);
}


bool CgroupsIsolatorProcess::supportsNesting()
{
  return true;
}


bool CgroupsIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Nothing> CgroupsIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Recover active containers first.
  vector<Future<Nothing>> recovers;
  foreach (const ContainerState& state, states) {
    // If we are a nested container with shared cgroups, we do not
    // need to recover anything since its ancestor will have cgroups
    // created for them.
    const bool shareCgroups =
      (state.has_container_info() &&
       state.container_info().has_linux_info() &&
       state.container_info().linux_info().has_share_cgroups())
        ? state.container_info().linux_info().share_cgroups()
        : true;

    if (state.container_id().has_parent() && shareCgroups) {
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
    const vector<Future<Nothing>>& futures)
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

      // Need to parse the cgroup to see if it's one we created (i.e.,
      // matches our separator structure) or one that someone else
      // created (e.g., in the future we might have nested containers
      // that are managed by something else rooted within the cgroup
      // hierarchy).
      Option<ContainerID> containerId =
        containerizer::paths::parseCgroupPath(flags.cgroups_root, cgroup);

      if (containerId.isNone()) {
        LOG(INFO) << "Not recovering cgroup " << cgroup;
        continue;
      }

      // Skip containerId which already have been recovered.
      if (infos.contains(containerId.get())) {
        continue;
      }

      if (orphans.contains(containerId.get())) {
        knownOrphans.insert(containerId.get());
      } else {
        unknownOrphans.insert(containerId.get());
      }
    }
  }

  vector<Future<Nothing>> recovers;

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
    const vector<Future<Nothing>>& futures)
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
  const string cgroup =
    containerizer::paths::getCgroupPath(flags.cgroups_root, containerId);

  vector<Future<Nothing>> recovers;
  hashset<string> recoveredSubsystems;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    if (!cgroups::exists(hierarchy, cgroup)) {
      // This may occur in two cases:
      // 1. If the executor has exited and the isolator has destroyed
      //    the cgroup but the agent dies before noticing this. This
      //    will be detected when the containerizer tries to monitor
      //    the executor's pid.
      // 2. After the agent recovery/upgrade, new cgroup subsystems
      //    are added to the agent cgroup isolation configuration.
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
    const vector<Future<Nothing>>& futures)
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
      containerizer::paths::getCgroupPath(flags.cgroups_root, containerId)));

  infos[containerId]->subsystems = recoveredSubsystems;

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // If the nested container shares cgroups with its parent container,
  // we don't need to prepare cgroups. In this case, the nested container
  // will inherit cgroups from its ancestor, so here we just
  // need to do the container-specific cgroups mounts.
  const bool shareCgroups =
    (containerConfig.has_container_info() &&
     containerConfig.container_info().has_linux_info() &&
     containerConfig.container_info().linux_info().has_share_cgroups())
      ? containerConfig.container_info().linux_info().share_cgroups()
      : true;

  if (containerId.has_parent() && shareCgroups) {
    return __prepare(containerId, containerConfig);
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  CHECK(containerConfig.container_class() != ContainerClass::DEBUG);

  CHECK(!containerId.has_parent() || !containerId.parent().has_parent())
    << "2nd level or greater nested cgroups are not supported";

  // We save 'Info' into 'infos' first so that even if 'prepare'
  // fails, we can properly cleanup the *side effects* created below.
  infos[containerId] = Owned<Info>(new Info(
      containerId,
      containerizer::paths::getCgroupPath(flags.cgroups_root, containerId)));

  vector<Future<Nothing>> prepares;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    string path = path::join(hierarchy, infos[containerId]->cgroup);

    VLOG(1) << "Creating cgroup at '" << path << "' "
            << "for container " << containerId;

    if (cgroups::exists(hierarchy, infos[containerId]->cgroup)) {
      return Failure("The cgroup at '" + path + "' already exists");
    }

    Try<Nothing> create = cgroups::create(
        hierarchy,
        infos[containerId]->cgroup,
        true);

    if (create.isError()) {
      return Failure(
          "Failed to create the cgroup at "
          "'" + path + "': " + create.error());
    }

    foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
      infos[containerId]->subsystems.insert(subsystem->name());
      prepares.push_back(subsystem->prepare(
          containerId,
          infos[containerId]->cgroup,
          containerConfig));
    }

    // Chown the cgroup so the executor or a nested container whose
    // `share_cgroups` is false can create nested cgroups. Do
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
    const vector<Future<Nothing>>& futures)
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

  return update(
      containerId,
      containerConfig.resources(),
      containerConfig.limits())
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::__prepare,
        containerId,
        containerConfig));
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::__prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // We will do container-specific cgroups mounts
  // only for the container with rootfs.
  if (!containerConfig.has_rootfs()) {
    return None();
  }

  ContainerLaunchInfo launchInfo;
  launchInfo.add_clone_namespaces(CLONE_NEWNS);

  // For the comounted subsystems (e.g., cpu & cpuacct, net_cls & net_prio),
  // we need to create a symbolic link for each of them to the mount point.
  // E.g.: ln -s /sys/fs/cgroup/cpu,cpuacct /sys/fs/cgroup/cpu
  //       ln -s /sys/fs/cgroup/cpu,cpuacct /sys/fs/cgroup/cpuacct
  foreach (const string& hierarchy, subsystems.keys()) {
    if (subsystems.get(hierarchy).size() > 1) {
      foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
        *launchInfo.add_file_operations() = containerSymlinkOperation(
            path::join("/sys/fs/cgroup", Path(hierarchy).basename()),
            path::join(
              containerConfig.rootfs(),
              "/sys/fs/cgroup",
              subsystem->name()));
      }
    }
  }

  // For the subsystem loaded by this isolator, do the container-specific
  // cgroups mount, e.g.:
  //   mount --bind /sys/fs/cgroup/memory/mesos/<containerId> /sys/fs/cgroup/memory // NOLINT(whitespace/line_length)
  Owned<Info> info = findCgroupInfo(containerId);
  if (!info.get()) {
    return Failure(
        "Failed to find cgroup for container " +
        stringify(containerId));
  }

  foreach (const string& hierarchy, subsystems.keys()) {
    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        path::join(hierarchy, info->cgroup),
        path::join(
            containerConfig.rootfs(),
            "/sys/fs/cgroup",
            Path(hierarchy).basename()),
        MS_BIND | MS_REC);
  }

  // Linux launcher will create freezer and systemd cgroups for the container,
  // so here we need to do the container-specific cgroups mounts for them too,
  // see MESOS-9070 for details.
  if (flags.launcher == "linux") {
    Result<string> hierarchy = cgroups::hierarchy("freezer");
    if (hierarchy.isError()) {
      return Failure(
          "Failed to retrieve the 'freezer' subsystem hierarchy: " +
          hierarchy.error());
    }

    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        path::join(
            hierarchy.get(),
            flags.cgroups_root,
            containerizer::paths::buildPath(
                containerId,
                CGROUP_SEPARATOR,
                containerizer::paths::JOIN)),
        path::join(
            containerConfig.rootfs(),
            "/sys/fs/cgroup",
            "freezer"),
        MS_BIND | MS_REC);

    if (systemd::enabled()) {
      *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
          path::join(
              systemd::hierarchy(),
              flags.cgroups_root,
              containerizer::paths::buildPath(
                  containerId,
                  CGROUP_SEPARATOR,
                  containerizer::paths::JOIN)),
          path::join(
              containerConfig.rootfs(),
              "/sys/fs/cgroup",
              "systemd"),
          MS_BIND | MS_REC);
    }
  }

  // TODO(qianzhang): This is a hack to pass the container-specific cgroups
  // mounts and the symbolic links to the command executor to do for the
  // command task. The reasons that we do it in this way are:
  //   1. We need to ensure the container-specific cgroups mounts are done
  //      only in the command task's mount namespace but not in the command
  //      executor's mount namespace.
  //   2. Even it's acceptable to do the container-specific cgroups mounts
  //      in the command executor's mount namespace and the command task
  //      inherit them from there (i.e., here we just return `launchInfo`
  //      rather than passing it via `--task_launch_info`), the container
  //      specific cgroups mounts will be hidden by the `sysfs` mounts done in
  //      `mountSpecialFilesystems()` when the command executor launches the
  //      command task.
  if (containerConfig.has_task_info()) {
    ContainerLaunchInfo _launchInfo;

    _launchInfo.mutable_command()->add_arguments(
        "--task_launch_info=" +
        stringify(JSON::protobuf(launchInfo)));

    return _launchInfo;
  }

  return launchInfo;
}


Future<Nothing> CgroupsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  // We currently can't call `subsystem->isolate()` on nested
  // containers with shared cgroups, because we don't call `prepare()`,
  // `recover()`, or `cleanup()` on them either. If we were to call
  // `isolate()` on them, the call would likely fail because the subsystem
  // doesn't know about the container.
  //
  // TODO(klueska): In the future we should revisit this to make
  // sure that doing things this way is sufficient (or otherwise
  // update our invariants to allow us to call this here).

  vector<Future<Nothing>> isolates;

  if (infos.contains(containerId)) {
    foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
      isolates.push_back(subsystem->isolate(
          containerId,
          infos[containerId]->cgroup,
          pid));
    }
  }

  // We isolate all the subsystems first so that the subsystem is
  // fully configured before assigning the process. This improves
  // the atomicity of the assignment for any system processes that
  // observe it.
  return await(isolates)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_isolate,
        lambda::_1,
        containerId,
        pid));
}


Future<Nothing> CgroupsIsolatorProcess::_isolate(
    const vector<Future<Nothing>>& futures,
    const ContainerID& containerId,
    pid_t pid)
{
  vector<string> errors;

  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      errors.push_back((future.isFailed()
          ? future.failure()
          : "discarded"));
    }
  }

  if (!errors.empty()) {
    return Failure(
        "Failed to isolate subsystems: " +
        strings::join(";", errors));
  }

  // If we are a nested container with shared cgroups,
  // we inherit the cgroup from our parent.
  Owned<Info> info = findCgroupInfo(containerId);
  if (!info.get()) {
    return Failure(
        "Failed to find cgroup for container " +
        stringify(containerId));
  }

  const string& cgroup = info->cgroup;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    // If new cgroup subsystems are added after the agent
    // upgrade, the newly added cgroup subsystems do not
    // exist on old container's cgroup hierarchy. So skip
    // assigning the pid to this cgroup subsystem.
    if (containerId.has_parent() && containerId != info->containerId &&
        !cgroups::exists(hierarchy, cgroup)) {
      LOG(INFO) << "Skipping assigning pid " << stringify(pid)
                << " to cgroup at '" << path::join(hierarchy, cgroup)
                << "' for container " << containerId
                << " because its parent container " << containerId.parent()
                << " does not have this cgroup hierarchy";
      continue;
    }

    Try<Nothing> assign = cgroups::assign(
        hierarchy,
        cgroup,
        pid);

    if (assign.isError()) {
      string message =
        "Failed to assign container " + stringify(containerId) +
        " pid " + stringify(pid) + " to cgroup at '" +
        path::join(hierarchy, cgroup) + "': " + assign.error();

      LOG(ERROR) << message;

      return Failure(message);
    }
  }

  return Nothing();
}


Future<ContainerLimitation> CgroupsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // Since we do not maintain cgroups for nested containers with shared
  // cgroups directly, we simply return a pending future here, indicating
  // that the limit for the nested container will never be reached.
  if (!infos.contains(containerId)) {
    // TODO(abudnik): We should return a failure for the nested container
    // whose `share_cgroups` is false but `infos` does not contain it.
    // This may happen due to a bug in our code.
    //
    // NOTE: We return a pending future for a non-existent nested container
    // whose ancestor is known to the cgroups isolator.
    if (findCgroupInfo(containerId).get()) {
      return Future<ContainerLimitation>();
    } else {
      return Failure("Unknown container");
    }
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
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  vector<Future<Nothing>> updates;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      updates.push_back(subsystem->update(
          containerId,
          infos[containerId]->cgroup,
          resourceRequests,
          resourceLimits));
    }
  }

  return await(updates)
    .then(defer(
        PID<CgroupsIsolatorProcess>(this),
        &CgroupsIsolatorProcess::_update,
        lambda::_1));
}


Future<Nothing> CgroupsIsolatorProcess::_update(
    const vector<Future<Nothing>>& futures)
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
        strings::join("; ", errors));
  }

  return Nothing();
}


Future<ResourceStatistics> CgroupsIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  vector<Future<ResourceStatistics>> usages;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      usages.push_back(subsystem->usage(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(usages)
    .then([containerId](const vector<Future<ResourceStatistics>>& _usages) {
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
  // If we are a nested container unknown to the isolator,
  // we try to find the status of its ancestor.
  if (!infos.contains(containerId)) {
    // TODO(abudnik): We should return a failure for the nested container
    // whose `share_cgroups` is false but `infos` does not contain it.
    // This may happen due to a bug in our code.
    if (containerId.has_parent()) {
      return status(containerId.parent());
    } else {
      return Failure("Unknown container");
    }
  }

  vector<Future<ContainerStatus>> statuses;
  foreachvalue (const Owned<Subsystem>& subsystem, subsystems) {
    if (infos[containerId]->subsystems.contains(subsystem->name())) {
      statuses.push_back(subsystem->status(
          containerId,
          infos[containerId]->cgroup));
    }
  }

  return await(statuses)
    .then([containerId](const vector<Future<ContainerStatus>>& _statuses) {
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

  vector<Future<Nothing>> cleanups;
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
    const vector<Future<Nothing>>& futures)
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

  vector<Future<Nothing>> destroys;

  // TODO(haosdent): Use foreachkey once MESOS-5037 is resolved.
  foreach (const string& hierarchy, subsystems.keys()) {
    foreach (const Owned<Subsystem>& subsystem, subsystems.get(hierarchy)) {
      if (infos[containerId]->subsystems.contains(subsystem->name())) {
        destroys.push_back(cgroups::destroy(
            hierarchy,
            infos[containerId]->cgroup,
            flags.cgroups_destroy_timeout));

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
    const vector<Future<Nothing>>& futures)
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


Owned<CgroupsIsolatorProcess::Info> CgroupsIsolatorProcess::findCgroupInfo(
    const ContainerID& containerId) const
{
  Option<ContainerID> current = containerId;
  while (current.isSome()) {
    Option<Owned<Info>> info = infos.get(current.get());
    if (info.isSome()) {
      return info.get();
    }

    if (!current->has_parent()) {
      break;
    }

    current = current->parent();
  }

  return nullptr;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
