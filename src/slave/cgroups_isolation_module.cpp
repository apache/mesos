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

#include <signal.h>
#include <unistd.h>

#include <sys/file.h> // For flock.
#include <sys/types.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "common/units.hpp"

#include "linux/cgroups.hpp"

#include "slave/cgroups_isolation_module.hpp"

using process::defer;
using process::Future;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

const size_t CPU_SHARES_PER_CPU = 1024;
const size_t MIN_CPU_SHARES = 10;
const size_t MIN_MEMORY_MB = 32 * Megabyte;


CgroupsIsolationModule::CgroupsIsolationModule()
  : ProcessBase(ID::generate("cgroups-isolation-module")),
    initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addProcessExitedListener, this);
}


CgroupsIsolationModule::~CgroupsIsolationModule()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  process::wait(reaper); // Necessary for disambiguation.
  delete reaper;
}


void CgroupsIsolationModule::initialize(
    const Flags& _flags,
    bool _local,
    const PID<Slave>& _slave)
{
  flags = _flags;
  local = _local;
  slave = _slave;

  // Make sure that cgroups is enabled by the kernel.
  if (!cgroups::enabled()) {
    std::cerr << "No cgroups support detected on this kernel" << std::endl;
    abort();
  }

  // Make sure that we have root permissions.
  if (geteuid() != 0) {
    std::cerr << "Using cgroups requires root permissions" << std::endl;
    abort();
  }

  // Configure cgroups hierarchy root path.
  hierarchy = flags.cgroups_hierarchy_root;

  LOG(INFO) << "Using " << hierarchy << " as cgroups hierarchy root";

  // Configure required/optional subsystems.
  hashset<string> requiredSubsystems;
  hashset<string> optionalSubsystems;

  requiredSubsystems.insert("cpu");
  requiredSubsystems.insert("memory");
  requiredSubsystems.insert("freezer");

  optionalSubsystems.insert("cpuset");
  optionalSubsystems.insert("blkio");

  // Probe cgroups subsystems.
  hashset<string> enabledSubsystems;
  hashset<string> busySubsystems;

  Try<set<string> > enabled = cgroups::subsystems();
  if (enabled.isError()) {
    LOG(FATAL) << "Failed to probe cgroups subsystems: " << enabled.error();
  } else {
    foreach (const string& name, enabled.get()) {
      enabledSubsystems.insert(name);

      Try<bool> busy = cgroups::busy(name);
      if (busy.isError()) {
        LOG(FATAL) << "Failed to probe cgroups subsystems: " << busy.error();
      }

      if (busy.get()) {
        busySubsystems.insert(name);
      }
    }
  }

  // Make sure that all the required subsystems are enabled by the kernel.
  foreach (const string& name, requiredSubsystems) {
    if (!enabledSubsystems.contains(name)) {
      LOG(FATAL) << "Required subsystem " << name
                 << " is not enabled by the kernel";
    }
  }

  // Prepare the cgroups hierarchy root.
  if (cgroups::checkHierarchy(hierarchy).isError()) {
    // The given hierarchy is not a cgroups hierarchy root. We will try to
    // create a cgroups hierarchy root there.
    if (os::exists(hierarchy)) {
      // The path specified by the given hierarchy already exists in the file
      // system. We try to remove it if it is an empty directory. This will
      // helps us better deal with slave reboots as we don't need to manually
      // remove the residue directory after a slave reboot.
      if (::rmdir(hierarchy.c_str()) < 0) {
        LOG(FATAL) << "Cannot create cgroups hierarchy root at " << hierarchy
                   << ". Consider removing it";
      }
    }

    // The comma-separated subsystem names which will be passed to
    // cgroups::createHierarchy to create the hierarchy root.
    string subsystems;

    // Make sure that all the required subsystems are not busy so that we can
    // activate them in the given cgroups hierarchy root.
    foreach (const string& name, requiredSubsystems) {
      if (busySubsystems.contains(name)) {
        LOG(FATAL) << "Required subsystem " << name << " is busy";
      }

      subsystems.append(name + ",");
    }

    // Also activate those optional subsystems that are not busy.
    foreach (const string& name, optionalSubsystems) {
      if (enabledSubsystems.contains(name) && !busySubsystems.contains(name)) {
        subsystems.append(name + ",");
      }
    }

    // Create the cgroups hierarchy root.
    Try<Nothing> create =
        cgroups::createHierarchy(hierarchy, strings::trim(subsystems, ","));
    if (create.isError()) {
      LOG(FATAL) << "Failed to create cgroups hierarchy root at " << hierarchy
                 << ": " << create.error();
    }
  }

  // Probe activated subsystems in the cgroups hierarchy root.
  Try<set<string> > activated = cgroups::subsystems(hierarchy);
  foreach (const string& name, activated.get()) {
    activatedSubsystems.insert(name);
  }

  // Make sure that all the required subsystems are activated.
  foreach (const string& name, requiredSubsystems) {
    if (!activatedSubsystems.contains(name)) {
      LOG(FATAL) << "Required subsystem " << name
                 << " is not activated in hierarchy " << hierarchy;
    }
  }

  // Create the root "mesos" cgroup if it doesn't exist.
  if (cgroups::checkCgroup(hierarchy, "mesos").isError()) {
    // No root cgroup exists, create it.
    Try<Nothing> create = cgroups::createCgroup(hierarchy, "mesos");
    CHECK(create.isSome())
      << "Failed to create the \"mesos\" cgroup: "
      << create.error();
  }

  // Make sure this kernel supports creating nested cgroups.
  Try<Nothing> create = cgroups::createCgroup(hierarchy, "mesos/test");
  if (create.isError()) {
    std::cerr << "Failed to create a nested \"test\" cgroup, your kernel "
              << "might be too old to use the cgroups isolation module: "
              << create.error() << std::endl;
    abort();
  }

  Try<Nothing> remove = cgroups::removeCgroup(hierarchy, "mesos/test");
  CHECK(remove.isSome())
    << "Failed to remove the nested \"test\" cgroup:" << remove.error();

  // Try and put an _advisory_ file lock on the tasks' file of our
  // root cgroup to check and see if another slave is already running.
  Try<int> fd = os::open(path::join(hierarchy, "mesos", "tasks"), O_RDONLY);
  CHECK(fd.isSome());
  Try<Nothing> cloexec = os::cloexec(fd.get());
  CHECK(cloexec.isSome());
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    std::cerr << "Another mesos-slave appears to be running!" << std::endl;
    abort();
  }

  // Cleanup any orphaned cgroups created in previous executions (this
  // should be safe because we've been able to acquire the file lock).
  Try<vector<string> > cgroups = cgroups::getCgroups(hierarchy, "mesos");
  CHECK(cgroups.isSome())
    << "Failed to get nested cgroups of \"mesos\": "
    << cgroups.error();
  foreach (const string& cgroup, cgroups.get()) {
    LOG(INFO) << "Removing orphaned cgroup '" << cgroup << "'";
    cgroups::destroyCgroup(hierarchy, cgroup)
      .onAny(defer(PID<CgroupsIsolationModule>(this),
                   &CgroupsIsolationModule::destroyWaited,
                   cgroup,
                   lambda::_1));
  }

  // Make sure the kernel supports OOM controls.
  Try<Nothing> check =
    cgroups::checkControl(hierarchy, "mesos", "memory.oom_control");
  if (check.isError()) {
    std::cerr << "Failed to find 'memory.oom_control', your kernel "
              << "might be too old to use the cgroups isolation module: "
              << check.error() << std::endl;
    abort();
  }

  // Disable the OOM killer so that we can capture 'memory.stat'.
  Try<Nothing> disable =
    cgroups::writeControl(hierarchy, "mesos", "memory.oom_control", "1");
  CHECK(disable.isSome())
    << "Failed to disable OOM killer: " << disable.error();

  // Configure resource changed handlers. We only add handlers for
  // resources that have the appropriate subsystem activated.
  if (activatedSubsystems.contains("cpu")) {
    handlers["cpus"] = &CgroupsIsolationModule::cpusChanged;
  }

  if (activatedSubsystems.contains("memory")) {
    handlers["mem"] = &CgroupsIsolationModule::memChanged;
  }

  initialized = true;
}


void CgroupsIsolationModule::launchExecutor(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot launch executors before initialization";

  const ExecutorID& executorId = executorInfo.executor_id();

  // Register the cgroup information.
  CgroupInfo* info = registerCgroupInfo(frameworkId, executorId);

  LOG(INFO) << "Launching " << executorId
            << " (" << executorInfo.command().value() << ")"
            << " in " << directory
            << " with resources " << resources
            << " for framework " << frameworkId
            << " in cgroup " << info->name();

  // First fetch the executor.
  launcher::ExecutorLauncher launcher(
      frameworkId,
      executorInfo.executor_id(),
      executorInfo.command(),
      frameworkInfo.user(),
      directory,
      slave,
      flags.frameworks_home,
      flags.hadoop_home,
      !local,
      flags.switch_user,
      "");

  if (launcher.setup() < 0) {
    LOG(ERROR) << "Error setting up executor " << executorId
               << " for framework " << frameworkId;

    unregisterCgroupInfo(frameworkId, executorId);

    LOG(INFO) << "Telling slave of lost executor " << executorId
              << " of framework " << frameworkId;

    dispatch(slave,
             &Slave::executorExited,
             frameworkId,
             executorId,
             -1); // TODO(benh): Determine "correct" status.

    return;
  }

  // Create a new cgroup for the executor.
  Try<Nothing> create = cgroups::createCgroup(hierarchy, info->name());
  if (create.isError()) {
    LOG(FATAL) << "Failed to create cgroup for executor " << executorId
               << " of framework " << frameworkId
               << ": " << create.error();
  }

  // Setup the initial resource constrains.
  resourcesChanged(frameworkId, executorId, resources);

  // Start listening on OOM events.
  oomListen(frameworkId, executorId);

  // Launch the executor using fork-exec.
  pid_t pid;
  if ((pid = ::fork()) == -1) {
    LOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    // In parent process.
    LOG(INFO) << "Forked executor at = " << pid;

    // Store the pid of the leading process of the executor.
    info->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave,
             &Slave::executorStarted,
             frameworkId,
             executorId,
             pid);
  } else {
    // In child process.
    // Put self into the newly created cgroup.
    Try<Nothing> assign =
      cgroups::assignTask(hierarchy, info->name(), ::getpid());
    if (assign.isError()) {
      LOG(FATAL) << "Failed to assign for executor " << executorId
                 << " of framework " << frameworkId
                 << ": " << assign.error();
    }

    // Now launch the executor (this function should not return).
    launcher.launch();
  }
}


void CgroupsIsolationModule::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(initialized) << "Cannot kill executors before initialization";

  CgroupInfo* info = findCgroupInfo(frameworkId, executorId);
  if (info == NULL || info->killed) {
    LOG(ERROR) << "Asked to kill an unknown/killed executor!";
    return;
  }

  LOG(INFO) << "Killing executor " << executorId
            << " of framework " << frameworkId;

  // Stop the OOM listener if needed.
  if (info->oomNotifier.isPending()) {
    info->oomNotifier.discard();
  }

  // Destroy the cgroup that is associated with the executor. Here, we don't
  // wait for it to succeed as we don't want to block the isolation module.
  // Instead, we register a callback which will be invoked when its result is
  // ready.
  cgroups::destroyCgroup(hierarchy, info->name())
    .onAny(defer(PID<CgroupsIsolationModule>(this),
                 &CgroupsIsolationModule::destroyWaited,
                 info->name(),
                 lambda::_1));

  // We do not unregister the cgroup info here, instead, we ask the process
  // exit handler to unregister the cgroup info.
  info->killed = true;
}


void CgroupsIsolationModule::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot change resources before initialization";

  CgroupInfo* info = findCgroupInfo(frameworkId, executorId);
  if (info == NULL || info->killed) {
    LOG(INFO) << "Asked to update resources for an unknown/killed executor";
    return;
  }

  LOG(INFO) << "Changing cgroup controls for executor " << executorId
            << " of framework " << frameworkId
            << " with resources " << resources;

  // For each resource, invoke the corresponding handler.
  foreach (const Resource& resource, resources) {
    if (handlers.contains(resource.name())) {
      Try<Nothing> result = (this->*handlers[resource.name()])(info, resource);
      if (result.isError()) {
        LOG(ERROR) << result.error();
      }
    }
  }
}


void CgroupsIsolationModule::processExited(pid_t pid, int status)
{
  CgroupInfo* info = findCgroupInfo(pid);
  if (info != NULL) {
    FrameworkID frameworkId = info->frameworkId;
    ExecutorID executorId = info->executorId;

    LOG(INFO) << "Telling slave of lost executor " << executorId
              << " of framework " << frameworkId;

    dispatch(slave,
             &Slave::executorExited,
             frameworkId,
             executorId,
             status);

    if (!info->killed) {
      killExecutor(frameworkId, executorId);
    }

    unregisterCgroupInfo(frameworkId, executorId);
  }
}


Try<Nothing> CgroupsIsolationModule::cpusChanged(
    const CgroupInfo* info,
    const Resource& resource)
{
  CHECK(resource.name() == "cpus");

  if (resource.type() != Value::SCALAR) {
    return Try<Nothing>::error("Expecting resource 'cpus' to be a scalar");
  }

  double cpus = resource.scalar().value();
  size_t cpuShares =
    std::max((size_t)(CPU_SHARES_PER_CPU * cpus), MIN_CPU_SHARES);

  Try<Nothing> write = cgroups::writeControl(
      hierarchy, info->name(), "cpu.shares", stringify(cpuShares));

  if (write.isError()) {
    return Try<Nothing>::error(
        "Failed to update 'cpu.shares': " + write.error());
  }

  LOG(INFO) << "Updated 'cpu.shares' to " << cpuShares
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  return Nothing();
}


Try<Nothing> CgroupsIsolationModule::memChanged(
    const CgroupInfo* info,
    const Resource& resource)
{
  CHECK(resource.name() == "mem");

  if (resource.type() != Value::SCALAR) {
    return Try<Nothing>::error("Expecting resource 'mem' to be a scalar");
  }

  double mem = resource.scalar().value();
  size_t limitInBytes =
    std::max((size_t) mem, MIN_MEMORY_MB) * 1024LL * 1024LL;

  // Determine which control to set. If this is the first time we're
  // setting the limit, use 'memory.limit_in_bytes'. The "first time"
  // is determined by checking whether or not we've forked a process
  // in the cgroup yet (i.e., 'info->pid != -1'). If this is not the
  // first time we're setting the limit AND we're decreasing the
  // limit, use 'memory.soft_limit_in_bytes'. We do this because we
  // might not be able to decrease 'memory.limit_in_bytes' if too much
  // memory is being used. This is probably okay if the machine has
  // available resources; TODO(benh): Introduce a MemoryWatcherProcess
  // which monitors the descrepancy between usage and soft limit and
  // introduces a "manual oom" if necessary.
  string control = "memory.limit_in_bytes";

  if (info->pid != -1) {
    Try<string> read = cgroups::readControl(
        hierarchy, info->name(), "memory.limit_in_bytes");
    if (read.isError()) {
      return Try<Nothing>::error(
          "Failed to read 'memory.limit_in_bytes': " + read.error());
    }

    Try<size_t> currentLimitInBytes = numify<size_t>(strings::trim(read.get()));
    CHECK(currentLimitInBytes.isSome()) << currentLimitInBytes.error();

    if (limitInBytes <= currentLimitInBytes.get()) {
      control = "memory.soft_limit_in_bytes";
    }
  }

  Try<Nothing> write = cgroups::writeControl(
      hierarchy, info->name(), control, stringify(limitInBytes));

  if (write.isError()) {
    return Try<Nothing>::error(
        "Failed to update '" + control + "': " + write.error());
  }

  LOG(INFO) << "Updated '" << control << "' to " << limitInBytes
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  return Nothing();
}


void CgroupsIsolationModule::oomListen(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CgroupInfo* info = findCgroupInfo(frameworkId, executorId);
  CHECK(info != NULL) << "Cgroup info is not registered";

  info->oomNotifier =
    cgroups::listenEvent(hierarchy, info->name(), "memory.oom_control");

  // If the listening fails immediately, something very wrong happened.
  // Therefore, we report a fatal error here.
  if (info->oomNotifier.isFailed()) {
    LOG(FATAL) << "Failed to listen for OOM events for executor " << executorId
               << " of framework " << frameworkId
               << ": "<< info->oomNotifier.failure();
  }

  LOG(INFO) << "Started listening for OOM events for executor " << executorId
            << " of framework " << frameworkId;

  info->oomNotifier.onAny(
      defer(PID<CgroupsIsolationModule>(this),
            &CgroupsIsolationModule::oomWaited,
            frameworkId,
            executorId,
            info->tag,
            lambda::_1));
}


void CgroupsIsolationModule::oomWaited(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& tag,
    const Future<uint64_t>& future)
{
  LOG(INFO) << "OOM notifier is triggered for executor "
            << executorId << " of framework " << frameworkId
            << " with tag " << tag;

  if (future.isDiscarded()) {
    LOG(INFO) << "Discarded OOM notifier for executor "
              << executorId << " of framework " << frameworkId
              << " with tag " << tag;
  } else if (future.isFailed()) {
    LOG(ERROR) << "Listening on OOM events failed for executor "
               << executorId << " of framework " << frameworkId
               << " with tag " << tag << ": " << future.failure();
  } else {
    // Out-of-memory event happened, call the handler.
    oom(frameworkId, executorId, tag);
  }
}


void CgroupsIsolationModule::oom(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& tag)
{
  CgroupInfo* info = findCgroupInfo(frameworkId, executorId);
  if (info == NULL) {
    // It is likely that processExited is executed before this function (e.g.
    // The kill and OOM events happen at the same time, and the process exit
    // event arrives first.) Therefore, we should not report a fatal error here.
    LOG(INFO) << "OOM detected for an exited executor";
    return;
  }

  // To safely ignore the OOM event from the previous launch of the same
  // executor (with the same frameworkId and executorId).
  if (tag != info->tag) {
    LOG(INFO) << "OOM detected for the previous launch of the same executor";
    return;
  }

  // If killed is set, the OOM notifier will be discarded in oomWaited.
  // Therefore, we should not be able to reach this point.
  CHECK(!info->killed) << "OOM detected for a killed executor";

  LOG(INFO) << "OOM detected for executor " << executorId
            << " of framework " << frameworkId
            << " with tag " << tag;

  // Output 'memory.limit_in_bytes' of the cgroup to help with debugging.
  Try<string> read =
    cgroups::readControl(hierarchy, info->name(), "memory.limit_in_bytes");
  if (read.isSome()) {
    LOG(INFO) << "MEMORY LIMIT: " << strings::trim(read.get()) << " bytes";
  }

  // Output 'memory.usage_in_bytes'.
  read = cgroups::readControl(hierarchy, info->name(), "memory.usage_in_bytes");
  if (read.isSome()) {
    LOG(INFO) << "MEMORY USAGE: " << strings::trim(read.get()) << " bytes";
  }

  // Output 'memory.stat' of the cgroup to help with debugging.
  read = cgroups::readControl(hierarchy, info->name(), "memory.stat");
  if (read.isSome()) {
    LOG(INFO) << "MEMORY STATISTICS: \n" << read.get();
  }

  // TODO(jieyu): Have a mechanism to use a different policy (e.g. freeze the
  // executor) when OOM happens.
  killExecutor(frameworkId, executorId);
}


void CgroupsIsolationModule::destroyWaited(
    const string& cgroup,
    const Future<bool>& future)
{
  if (future.isReady()) {
    LOG(INFO) << "Successfully destroyed the cgroup " << cgroup;
  } else {
    LOG(FATAL) << "Failed to destroy the cgroup " << cgroup
               << ": " << future.failure();
  }
}


CgroupsIsolationModule::CgroupInfo* CgroupsIsolationModule::registerCgroupInfo(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CgroupInfo* info = new CgroupInfo;
  info->frameworkId = frameworkId;
  info->executorId = executorId;
  info->tag = UUID::random().toString();
  info->pid = -1;
  info->killed = false;
  infos[frameworkId][executorId] = info;
  return info;
}


void CgroupsIsolationModule::unregisterCgroupInfo(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (infos.contains(frameworkId)) {
    if (infos[frameworkId].contains(executorId)) {
      delete infos[frameworkId][executorId];
      infos[frameworkId].erase(executorId);
      if (infos[frameworkId].empty()) {
        infos.erase(frameworkId);
      }
    }
  }
}


CgroupsIsolationModule::CgroupInfo* CgroupsIsolationModule::findCgroupInfo(
    pid_t pid)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachvalue (CgroupInfo* info, infos[frameworkId]) {
      if (info->pid == pid) {
        return info;
      }
    }
  }
  return NULL;
}


CgroupsIsolationModule::CgroupInfo* CgroupsIsolationModule::findCgroupInfo(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (infos.find(frameworkId) != infos.end()) {
    if (infos[frameworkId].find(executorId) != infos[frameworkId].end()) {
      return infos[frameworkId][executorId];
    }
  }
  return NULL;
}

} // namespace mesos {
} // namespace internal {
} // namespace slave {
