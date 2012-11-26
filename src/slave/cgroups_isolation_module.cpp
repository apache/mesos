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

#include <stout/exit.hpp>
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
using std::ostringstream;
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
    EXIT(1) << "No cgroups support detected in this kernel";
  }

  // Make sure that we have root permissions.
  if (geteuid() != 0) {
    EXIT(1) << "Using cgroups requires root permissions";
  }

  // Configure cgroups hierarchy root path.
  hierarchy = flags.cgroups_hierarchy_root;

  LOG(INFO) << "Using " << hierarchy << " as cgroups hierarchy root";

  // Determine desired subsystems.
  foreach (const string& subsystem,
           strings::tokenize(flags.cgroups_subsystems, ",")) {
    // TODO(benh): Implement a 'sets::union' that takes a vector or
    // set rather than looping here!
    subsystems.insert(subsystem);
  }

  // Regardless of whether or not it was destired, we require the
  // 'freezer' subsystem in order to destroy a cgroup.
  subsystems.insert("freezer");

  // Check if the hierarchy is already mounted, and if not, mount it.
  Try<bool> mounted = cgroups::mounted(hierarchy);
  if (mounted.isError()) {
    LOG(FATAL) << "Failed to determine if " << hierarchy
               << " is already mounted: " << mounted.error();
  } else if (mounted.get()) {
    // Make sure that all the desired subsystems are attached to the
    // already mounted hierarchy.
    Try<set<string> > attached = cgroups::subsystems(hierarchy);
    if (attached.isError()) {
      LOG(FATAL) << "Failed to determine the attached subsystems "
                 << "for the cgroup hierarchy at " << hierarchy << ": "
                 << attached.error();
    }
    foreach (const string& subsystem, subsystems) {
      if (attached.get().count(subsystem) == 0) {
        EXIT(1) << "The cgroups hierarchy at " << hierarchy
                << " can not be used because it does not have the '"
                << subsystem << "' subsystem attached";
      }
    }
  } else {
    // Attempt to mount the hierarchy ourselves.
    if (os::exists(hierarchy)) {
      // The path specified by the given hierarchy already exists in
      // the file system. We try to remove it if it is an empty
      // directory. This will helps us better deal with slave restarts
      // since we won't need to manually remove the directory.
      Try<Nothing> rmdir = os::rmdir(hierarchy, false);
      CHECK(rmdir.isSome())
        << "Failed to mount cgroups hierarchy at " << hierarchy
        << " because we could not remove existing directory: " << rmdir.error();
    }

    // Mount the cgroups hierarchy.
    Try<Nothing> mount = cgroups::mount(
        hierarchy, strings::join(",", subsystems));
    CHECK(mount.isSome())
      << "Failed to mount cgroups hierarchy at "
      << hierarchy << ": " << mount.error();
  }

  // Create the root "mesos" cgroup if it doesn't exist.
  Try<bool> exists = cgroups::exists(hierarchy, "mesos");
  CHECK(exists.isSome())
    << "Failed to determine if the \"mesos\" cgroup already exists "
    << "in the hierarchy at " << hierarchy << ": " << exists.error();
  if (!exists.get()) {
    // No root cgroup exists, create it.
    Try<Nothing> create = cgroups::create(hierarchy, "mesos");
    CHECK(create.isSome())
      << "Failed to create the \"mesos\" cgroup: " << create.error();
  }

  // Make sure this kernel supports creating nested cgroups.
  Try<Nothing> create = cgroups::create(hierarchy, "mesos/test");
  if (create.isError()) {
    EXIT(1) << "Failed to create a nested \"test\" cgroup. Your kernel "
            << "might be too old to use the cgroups isolation module: "
            << create.error();
  }

  Try<Nothing> remove = cgroups::remove(hierarchy, "mesos/test");
  CHECK(remove.isSome())
    << "Failed to remove the nested \"test\" cgroup:" << remove.error();

  // Try and put an _advisory_ file lock on the tasks' file of our
  // root cgroup to check and see if another slave is already running.
  Try<int> fd = os::open(path::join(hierarchy, "mesos", "tasks"), O_RDONLY);
  CHECK(fd.isSome());
  Try<Nothing> cloexec = os::cloexec(fd.get());
  CHECK(cloexec.isSome());
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    EXIT(1) << "Another mesos-slave appears to be running!";
  }

  // Cleanup any orphaned cgroups created in previous executions (this
  // should be safe because we've been able to acquire the file lock).
  Try<vector<string> > cgroups = cgroups::get(hierarchy, "mesos");
  CHECK(cgroups.isSome())
    << "Failed to get nested cgroups of \"mesos\": " << cgroups.error();
  foreach (const string& cgroup, cgroups.get()) {
    LOG(INFO) << "Removing orphaned cgroup '" << cgroup << "'";
    cgroups::destroy(hierarchy, cgroup)
      .onAny(defer(PID<CgroupsIsolationModule>(this),
                   &CgroupsIsolationModule::destroyWaited,
                   cgroup,
                   lambda::_1));
  }

  // Make sure the kernel supports OOM controls.
  exists = cgroups::exists(hierarchy, "mesos", "memory.oom_control");
  CHECK(exists.isSome())
    << "Failed to determine if 'memory.oom_control' control exists: "
    << exists.error();
  if (!exists.get()) {
    EXIT(1) << "Failed to find 'memory.oom_control', your kernel "
            << "might be too old to use the cgroups isolation module";
  }

  // Disable the OOM killer so that we can capture 'memory.stat'.
  Try<Nothing> write = cgroups::write(
      hierarchy, "mesos", "memory.oom_control", "1");
  CHECK(write.isSome())
    << "Failed to disable OOM killer: " << write.error();

  // Configure resource changed handlers. We only add handlers for
  // resources that have the appropriate subsystems attached.
  if (subsystems.contains("cpu")) {
    handlers["cpus"] = &CgroupsIsolationModule::cpusChanged;
  }

  if (subsystems.contains("memory")) {
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
             &Slave::executorTerminated,
             frameworkId,
             executorId,
             -1,  // TODO(benh): Determine "correct" status.
             false,
             "Error launching executor");

    return;
  }

  // Create a new cgroup for the executor.
  Try<Nothing> create = cgroups::create(hierarchy, info->name());
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
    Try<Nothing> assign = cgroups::assign(hierarchy, info->name(), ::getpid());
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
  cgroups::destroy(hierarchy, info->name())
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

    LOG(INFO) << "Telling slave of terminated executor " << executorId
              << " of framework " << frameworkId;

    // TODO(vinod): Consider sending this message when the cgroup is
    // completely destroyed (i.e., inside destroyWaited()).
    // The tricky bit is to get the exit 'status' of the executor process.
    dispatch(slave,
             &Slave::executorTerminated,
             info->frameworkId,
             info->executorId,
             status,
             info->destroyed,
             info->reason);

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

  Try<Nothing> write = cgroups::write(
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
    Try<string> read = cgroups::read(
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

  Try<Nothing> write = cgroups::write(
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
    cgroups::listen(hierarchy, info->name(), "memory.oom_control");

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
    LOG(INFO) << "OOM detected for an already terminated executor";
    return;
  }

  // We can also ignore an OOM event that we are late to process for a
  // previous instance of an executor.
  if (tag != info->tag) {
    LOG(INFO) << "OOM detected for a previous executor instance";
    return;
  }

  // If killed is set, the OOM notifier will be discarded in oomWaited.
  // Therefore, we should not be able to reach this point.
  CHECK(!info->killed) << "OOM detected for an already killed executor";

  LOG(INFO) << "OOM detected for executor " << executorId
            << " of framework " << frameworkId
            << " with tag " << tag;

  // Construct a "reason" string to describe why the isolation module
  // destroyed the executor's cgroup (in order to assist in debugging).
  ostringstream reason;

  Try<string> read = cgroups::read(
      hierarchy, info->name(), "memory.limit_in_bytes");
  if (read.isSome()) {
    reason << "MEMORY LIMIT: " << strings::trim(read.get()) << " bytes\n";
  }

  // Output 'memory.usage_in_bytes'.
  read = cgroups::read(hierarchy, info->name(), "memory.usage_in_bytes");
  if (read.isSome()) {
    reason << "MEMORY USAGE: " << strings::trim(read.get()) << " bytes\n";
  }

  // Output 'memory.stat' of the cgroup to help with debugging.
  read = cgroups::read(hierarchy, info->name(), "memory.stat");
  if (read.isSome()) {
    reason << "MEMORY STATISTICS: \n" << read.get() << "\n";
  }

  LOG(INFO) << strings::trim(reason.str()); // Trim the extra '\n' at the end.

  info->destroyed = true;
  info->reason = reason.str();

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
  info->destroyed = false;
  info->reason = "";
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
