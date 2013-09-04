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

#include <math.h> // For floor.
#include <signal.h>
#include <unistd.h>

#include <sys/file.h> // For flock.
#include <sys/types.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "common/units.hpp"

#include "linux/cgroups.hpp"

#include "slave/cgroups_isolator.hpp"
#include "slave/state.hpp"

using process::defer;
using process::Future;

using std::list;
using std::map;
using std::set;
using std::string;
using std::ostringstream;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;

// CPU subsystem constants.
const size_t CPU_SHARES_PER_CPU = 1024;
const size_t MIN_CPU_SHARES = 10;
const Duration CPU_CFS_PERIOD = Milliseconds(100); // Linux default.
const Duration MIN_CPU_CFS_QUOTA = Milliseconds(1);

// Memory subsystem constants.
const Bytes MIN_MEMORY = Megabytes(32);


// This is an approximate double precision equality check.
// It only considers up to 0.001 precision.
// This is used so that we can enforce correct arithmetic on "millicpu" units.
// TODO(bmahler): Banish this to hell when we expose individual cpus as a
// resource to frameworks, so that we can enforce having no fractions.
bool almostEqual(double d1, double d2) {
  return (d1 <= (d2 + 0.001)) && (d1 >= (d2 - 0.001));
}


map<proc::CPU, double> Cpuset::grow(
    double delta,
    const map<proc::CPU, double>& usage)
{
  // The technique used here is to allocate as much as possible to
  // each cpu that has availability, until we've allocated the delta.
  // Note that we examine the cpus in the same order every time, which
  // means we don't yet consider locality.
  map<proc::CPU, double> allocation;
  foreachpair (const proc::CPU& cpu, double used, usage) {
    // Are we done allocating?
    if (almostEqual(delta, 0.0)) {
      break;
    }

    // Allocate as much as possible to this CPU.
    if (!almostEqual(used, 1.0)) {
      double free = 1.0 - used;
      double allocated = std::min(delta, free);
      allocation[cpu] = allocated;
      delta -= allocated;
      cpus[cpu] += allocated;
    }
  }

  CHECK(almostEqual(delta, 0.0))
    << "Failed to grow the cpuset by " << delta << " cpus\n"
    << "  cpus: " << stringify(cpus) << "\n"
    << "  usage: " << stringify(usage);

  return allocation;
}


map<proc::CPU, double> Cpuset::shrink(double delta)
{
  // The technique used here is to free as much as possible from the
  // least allocated cpu. This means we'll avoid fragmenting as we're
  // constantly trying to remove cpus belonging to this Cpuset.
  map<proc::CPU, double> deallocation;
  while (!almostEqual(delta, 0.0)) {
    // Find the CPU to which we have the least allocated.
    Option<proc::CPU> least;
    foreachpair (const proc::CPU& cpu, double used, cpus) {
      if (least.isNone() || used <= cpus[least.get()]) {
        least = cpu;
      }
    }

    CHECK(least.isSome())
      << "Failed to shrink the cpuset by " << delta << " cpus\n"
      << "  cpus: " << stringify(cpus);

    // Deallocate as much as possible from the least allocated CPU.
    double used = cpus[least.get()];
    double deallocated = std::min(used, delta);
    deallocation[least.get()] = deallocated;
    delta -= deallocated;
    cpus[least.get()] -= deallocated;

    // Ensure this Cpuset never contains unallocated CPUs.
    if (almostEqual(cpus[least.get()], 0.0)) {
      cpus.erase(least.get());
    }
  }

  return deallocation;
}


double Cpuset::usage() const
{
  double total = 0.0;
  foreachvalue (double used, cpus) {
    total += used;
  }
  return total;
}


std::ostream& operator << (std::ostream& out, const Cpuset& cpuset)
{
  vector<unsigned int> cpus;
  foreachpair (const proc::CPU& cpu, double used, cpuset.cpus) {
    CHECK(!almostEqual(used, 0.0));
    cpus.push_back(cpu.id);
  }
  std::sort(cpus.begin(), cpus.end());

  return out << strings::join(",", cpus);
}


CgroupsIsolator::CgroupsIsolator()
  : ProcessBase(ID::generate("cgroups-isolator")),
    initialized(false),
    lockFile(None()) {}


void CgroupsIsolator::initialize(
    const Flags& _flags,
    const Resources& _resources,
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
  hierarchy = flags.cgroups_hierarchy;

  LOG(INFO) << "Using " << hierarchy << " as cgroups hierarchy root";

  // Determine desired subsystems.
  foreach (const string& subsystem,
           strings::tokenize(flags.cgroups_subsystems, ",")) {
    // TODO(benh): Implement a 'sets::union' that takes a vector or
    // set rather than looping here!
    subsystems.insert(subsystem);
  }

  // Regardless of whether or not it was desired, we require the
  // 'freezer' subsystem in order to destroy a cgroup.
  subsystems.insert("freezer");

  // We require the 'cpuacct' subsystem to perform resource monitoring.
  subsystems.insert("cpuacct");

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
      if (rmdir.isError()) {
        EXIT(1) << "Failed to mount cgroups hierarchy at '" << hierarchy
                << "' because we could not remove existing directory"
                << ": " << rmdir.error();
      }
    }

    // Mount the cgroups hierarchy.
    Try<Nothing> mount = cgroups::mount(
        hierarchy, strings::join(",", subsystems));

    if (mount.isError()) {
      EXIT(1) << "Failed to mount cgroups hierarchy at '" << hierarchy
              << "': " << mount.error();
    }
  }

  // Create the root cgroup if it doesn't exist.
  Try<bool> exists = cgroups::exists(hierarchy, flags.cgroups_root);
  CHECK_SOME(exists)
    << "Failed to determine if '"<< flags.cgroups_root << "' cgroup "
    << "already exists in the hierarchy at '" << hierarchy << "'";

  if (!exists.get()) {
    // No root cgroup exists, create it.
    Try<Nothing> create = cgroups::create(hierarchy, flags.cgroups_root);
    CHECK_SOME(create)
      << "Failed to create the '" << flags.cgroups_root << "' cgroup";
  }

  // Create the nested test cgroup if it doesn't exist.
  exists = cgroups::exists(
      hierarchy, path::join(flags.cgroups_root, "test"));
  CHECK_SOME(exists)
    << "Failed to determine if '"<< flags.cgroups_root << "/test'"
    << " nested cgroup already exists in the hierarchy at '"
    << hierarchy << "'";

  if (!exists.get()) {
    // Make sure this kernel supports creating nested cgroups.
    Try<Nothing> create =
      cgroups::create(hierarchy, path::join(flags.cgroups_root, "test"));

    if (create.isError()) {
      EXIT(1) << "Failed to create a nested 'test' cgroup. Your kernel "
        << "might be too old to use the cgroups isolator: "
        << create.error();
    }
  }

  // Remove the nested 'test' cgroup.
  Try<Nothing> remove =
    cgroups::remove(hierarchy, path::join(flags.cgroups_root, "test"));

  CHECK_SOME(remove) << "Failed to remove the nested 'test' cgroup";

  // Try and put an _advisory_ file lock on the tasks' file of our
  // root cgroup to check and see if another slave is already running.
  Try<int> open =
    os::open(path::join(hierarchy, flags.cgroups_root, "tasks"), O_RDONLY);

  CHECK_SOME(open);

  lockFile = open.get();
  Try<Nothing> cloexec = os::cloexec(lockFile.get());
  CHECK_SOME(cloexec);
  if (flock(lockFile.get(), LOCK_EX | LOCK_NB) != 0) {
    EXIT(1) << "Another mesos-slave appears to be running!";
  }

  // Make sure the kernel supports OOM controls.
  exists = cgroups::exists(
      hierarchy, flags.cgroups_root, "memory.oom_control");

  CHECK_SOME(exists)
    << "Failed to determine if 'memory.oom_control' control exists";

  if (!exists.get()) {
    EXIT(1) << "Failed to find 'memory.oom_control', your kernel "
            << "might be too old to use the cgroups isolator";
  }

  // Disable the OOM killer so that we can capture 'memory.stat'.
  Try<Nothing> write = cgroups::write(
      hierarchy, flags.cgroups_root, "memory.oom_control", "1");

  CHECK_SOME(write) << "Failed to disable OOM killer";

  if (subsystems.contains("cpu") && subsystems.contains("cpuset")) {
    EXIT(1) << "The use of both 'cpu' and 'cpuset' subsystems is not allowed.\n"
            << "Please use only one of:\n"
            << "  cpu:    When willing to share cpus for higher efficiency.\n"
            << "  cpuset: When cpu pinning is desired.";
  }

  // Configure resource changed handlers. We only add handlers for
  // resources that have the appropriate subsystems attached.
  if (subsystems.contains("cpu")) {
    handlers["cpus"] = &CgroupsIsolator::cpusChanged;
  }

  if (subsystems.contains("cpuset")) {
    // TODO(bmahler): Consider making a cgroups primitive helper to perform
    // cgroups list format -> list of ints / strings conversion.
    hashset<unsigned int> cgroupCpus;
    Try<string> cpuset =
      cgroups::read(hierarchy, flags.cgroups_root, "cpuset.cpus");

    CHECK_SOME(cpuset) << "Failed to read cpuset.cpus";
    cpuset = strings::trim(cpuset.get());

    // Parse from "0-2,7,12-14" to a set(0,1,2,7,12,13,14).
    foreach (string range, strings::tokenize(cpuset.get(), ",")) {
      range = strings::trim(range);

      if (strings::contains(range, "-")) {
        // Case startId-endId (e.g. 0-2 in 0-2,7,12-14).
        vector<string> startEnd = strings::split(range, "-");
        CHECK(startEnd.size() == 2)
          << "Failed to parse cpu range '" << range
          << "' from cpuset.cpus '" << cpuset.get() << "'";

        Try<unsigned int> start =
          numify<unsigned int>(strings::trim(startEnd[0]));
        Try<unsigned int> end =
          numify<unsigned int>(strings::trim(startEnd[1]));

        CHECK(start.isSome() && end.isSome())
          << "Failed to parse cpu range '" << range
          << "' from cpuset.cpus '" << cpuset.get() << "'";

        for (unsigned int i = start.get(); i <= end.get(); i++) {
          cgroupCpus.insert(i);
        }
      } else {
        // Case id (e.g. 7 in 0-2,7,12-14).
        Try<unsigned int> cpuId = numify<unsigned int>(range);

        CHECK_SOME(cpuId)
          << "Failed to parse cpu '" << range << "' from cpuset.cpus '"
          << cpuset.get()  << "'";

        cgroupCpus.insert(cpuId.get());
      }
    }

    Value::Scalar none;
    Value::Scalar cpusResource = _resources.get("cpus", none);
    if (cpusResource.value() > cgroupCpus.size()) {
      EXIT(1) << "You have specified " << cpusResource.value() << " cpus, but "
              << "this is more than allowed by the cgroup cpuset.cpus: "
              << cpuset.get();
    }

    // Initialize our cpu allocations.
    Try<list<proc::CPU> > cpus = proc::cpus();

    CHECK_SOME(cpus) << "Failed to extract CPUs from /proc/cpuinfo";

    foreach (const proc::CPU& cpu, cpus.get()) {
      if (this->cpus.size() >= cpusResource.value()) {
        break;
      }

      if (cgroupCpus.contains(cpu.id)) {
        LOG(INFO) << "Initializing cpu allocation for " << cpu;
        this->cpus[cpu] = 0.0;
      }
    }

    handlers["cpus"] = &CgroupsIsolator::cpusetChanged;
  }

  if (subsystems.contains("memory")) {
    handlers["mem"] = &CgroupsIsolator::memChanged;
  }

  // Add handlers for optional subsystem features.
  if (flags.cgroups_enable_cfs) {
    // Verify dependent subsystem is present and kernel supports CFS controls.
    if (!subsystems.contains("cpu")) {
      EXIT(1) << "The 'cfs' cgroups feature flag is dependent on the 'cpu' "
              << "subsystem.\n"
              << "Please enable the cpu subsystem to use the cfs feature.";
    }

    exists = cgroups::exists(hierarchy, flags.cgroups_root, "cpu.cfs_quota_us");

    CHECK_SOME(exists)
      << "Failed to determine if 'cpu.cfs_quota_us' control exists";

    if (!exists.get()) {
      EXIT(1) << "Failed to find 'cpu.cfs_quota_us'. Your kernel "
              << "might be too old to use the CFS cgroups feature";
    }

    // Make "cfsChanged" the cpu resource handler.
    // TODO(tdmackey): Allow multiple handlers per resource.
    handlers["cpus"] = &CgroupsIsolator::cfsChanged;
  }

  initialized = true;
}


void CgroupsIsolator::finalize()
{
  // Unlock the advisory file.
  CHECK_SOME(lockFile) << "Uninitialized file descriptor!";
  if (flock(lockFile.get(), LOCK_UN) != 0) {
    PLOG(FATAL)
      << "Failed to unlock advisory lock file '"
      << path::join(hierarchy, flags.cgroups_root, "tasks") << "'";
  }

  Try<Nothing> close = os::close(lockFile.get());
  if (close.isError()) {
    LOG(ERROR) << "Failed to close advisory lock file '"
               << path::join(hierarchy, flags.cgroups_root, "tasks")
               << "': " << close.error();
  }
}


void CgroupsIsolator::launchExecutor(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const UUID& uuid,
    const string& directory,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot launch executors before initialization";

  const ExecutorID& executorId = executorInfo.executor_id();

  // Register the cgroup information.
  CgroupInfo* info =
    registerCgroupInfo(frameworkId, executorId, uuid, None(), flags);

  LOG(INFO) << "Launching " << executorId
            << " (" << executorInfo.command().value() << ")"
            << " in " << directory
            << " with resources " << resources
            << " for framework " << frameworkId
            << " in cgroup " << info->name();

  // Create a new cgroup for the executor.
  Try<Nothing> create = cgroups::create(hierarchy, info->name());

  if (create.isError()) {
    LOG(FATAL) << "Failed to create cgroup for executor " << executorId
               << " of framework " << frameworkId
               << ": " << create.error();
  }

  // Setup the initial resource constraints.
  resourcesChanged(frameworkId, executorId, resources);

  // Start listening on OOM events.
  oomListen(frameworkId, executorId);

  // Use pipes to determine which child has successfully changed session.
  int pipes[2];
  if (pipe(pipes) < 0) {
    PLOG(FATAL) << "Failed to create a pipe";
  }

  // Set the FD_CLOEXEC flags on these pipes
  Try<Nothing> cloexec = os::cloexec(pipes[0]);
  CHECK_SOME(cloexec) << "Error setting FD_CLOEXEC on pipe[0]";

  cloexec = os::cloexec(pipes[1]);
  CHECK_SOME(cloexec) << "Error setting FD_CLOEXEC on pipe[1]";

  // Launch the executor using fork-exec.
  pid_t pid;
  if ((pid = ::fork()) == -1) {
    LOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid > 0) {
    os::close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      PLOG(FATAL) << "Failed to get child PID from pipe";
    }

    os::close(pipes[0]);

    // In parent process.
    LOG(INFO) << "Forked executor at = " << pid;

    // Store the pid of the leading process of the executor.
    info->pid = pid;

    reaper.monitor(pid)
      .onAny(defer(PID<CgroupsIsolator>(this),
                   &CgroupsIsolator::reaped,
                   pid,
                   lambda::_1));

    // Tell the slave this executor has started.
    dispatch(slave,
             &Slave::executorStarted,
             frameworkId,
             executorId,
             pid);
  } else {
    // In child process, we make cleanup easier by putting process
    // into it's own session. DO NOT USE GLOG!
    os::close(pipes[0]);

    // NOTE: We setsid() in a loop because setsid() might fail if another
    // process has the same process group id as the calling process.
    while ((pid = setsid()) == -1) {
      perror("Could not put executor in its own session");

      std::cout << "Forking another process and retrying ..." << std::endl;

      if ((pid = fork()) == -1) {
        perror("Failed to fork to launch executor");
        abort();
      }

      if (pid > 0) {
        // In parent process.
        exit(0);
      }
    }

    if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
      perror("Failed to write PID on pipe");
      abort();
    }

    os::close(pipes[1]);

    launcher::ExecutorLauncher launcher(
        slaveId,
        frameworkId,
        executorInfo.executor_id(),
        uuid,
        executorInfo.command(),
        frameworkInfo.user(),
        directory,
        flags.work_dir,
        slave,
        flags.frameworks_home,
        flags.hadoop_home,
        !local,
        flags.switch_user,
        frameworkInfo.checkpoint(),
        flags.recovery_timeout);

    // First fetch the executor.
    if (launcher.setup() < 0) {
      EXIT(1) << "Failed to setup executor '" << executorId
              << "' for framework " << frameworkId;
    }

    // Put self into the newly created cgroup.
    // Note that the memory used for setting up the executor
    // (launcher.setup()) is charged to the slave's cgroup and
    // not to the executor's cgroup. When we assign the executor
    // to the its own cgroup, below, its memory charge will start
    // at 0. For more details, refer to
    // http://www.kernel.org/doc/Documentation/cgroups/memory.txt
    Try<Nothing> assign = cgroups::assign(hierarchy, info->name(), ::getpid());

    if (assign.isError()) {
      EXIT(1) << "Failed to assign executor '" << executorId
              << "' of framework " << frameworkId
              << " to its own cgroup '" << path::join(hierarchy, info->name())
              << "' : " << assign.error();
    }

    // Now launch the executor (this function should not return).
    launcher.launch();
  }
}


void CgroupsIsolator::killExecutor(
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

  info->killed = true;

  // Destroy the cgroup that is associated with the executor. Here, we
  // don't wait for it to succeed as we don't want to block the
  // isolator. Instead, we register a callback which will be invoked
  // when its result is ready.
  cgroups::destroy(hierarchy, info->name())
    .onAny(defer(PID<CgroupsIsolator>(this),
                 &CgroupsIsolator::_killExecutor,
                 info,
                 lambda::_1));
}


void CgroupsIsolator::resourcesChanged(
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

  info->resources = resources;

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


Future<ResourceStatistics> CgroupsIsolator::usage(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId) ||
      infos[frameworkId][executorId]->killed) {
    return Future<ResourceStatistics>::failed("Unknown or killed executor");
  }

  // Get the number of clock ticks, used for cpu accounting.
  static long ticks = sysconf(_SC_CLK_TCK);

  PCHECK(ticks > 0) << "Failed to get sysconf(_SC_CLK_TCK)";

  CgroupInfo* info = infos[frameworkId][executorId];
  CHECK_NOTNULL(info);

  ResourceStatistics result;
  result.set_timestamp(Clock::now().secs());

  // Set the resource allocations.
  Option<Bytes> mem = info->resources.mem();
  if (mem.isSome()) {
    result.set_mem_limit_bytes(mem.get().bytes());
  }

  Option<double> cpus = info->resources.cpus();
  if (cpus.isSome()) {
    result.set_cpus_limit(cpus.get());
  }

  Try<hashmap<string, uint64_t> > stat =
    cgroups::stat(hierarchy, info->name(), "cpuacct.stat");

  if (stat.isError()) {
    return Future<ResourceStatistics>::failed(
        "Failed to read cpuacct.stat: " + stat.error());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g., cgroups::cpuacct::stat.
  if (stat.get().contains("user") && stat.get().contains("system")) {
    result.set_cpus_user_time_secs(
        (double) stat.get()["user"] / (double) ticks);
    result.set_cpus_system_time_secs(
        (double) stat.get()["system"] / (double) ticks);
  }

  stat = cgroups::stat(hierarchy, info->name(), "memory.stat");

  if (stat.isError()) {
    return Future<ResourceStatistics>::failed(
        "Failed to read memory.stat: " + stat.error());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g, cgroups::memory::stat.
  if (stat.get().contains("rss")) {
    result.set_mem_rss_bytes(stat.get()["rss"]);
  }

  // Add the cpu.stat information.
  stat = cgroups::stat(hierarchy, info->name(), "cpu.stat");

  if (stat.isError()) {
    return Future<ResourceStatistics>::failed(
        "Failed to read cpu.stat: " + stat.error());
  }

  if (stat.get().contains("nr_periods")) {
    result.set_cpus_nr_periods(
        (uint32_t) stat.get()["nr_periods"]);
  }

  if (stat.get().contains("nr_throttled")) {
    result.set_cpus_nr_throttled(
        (uint32_t) stat.get()["nr_throttled"]);
  }

  if (stat.get().contains("throttled_time")) {
    result.set_cpus_throttled_time_secs(
        Nanoseconds(stat.get()["throttled_time"]).secs());
  }

  return result;
}


Future<Nothing> CgroupsIsolator::recover(
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering isolator";

  hashset<std::string> cgroups; // Recovered cgroups.

  if (state.isSome()) {
    foreachvalue (const FrameworkState& framework, state.get().frameworks) {
      foreachvalue (const ExecutorState& executor, framework.executors) {
        LOG(INFO) << "Recovering executor '" << executor.id
                  << "' of framework " << framework.id;

        if (executor.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its info cannot be recovered";
          continue;
        }

        if (executor.latest.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its latest run cannot be recovered";
          continue;
        }

        // We are only interested in the latest run of the executor!
        const UUID& uuid = executor.latest.get();
        CHECK(executor.runs.contains(uuid));
        const RunState& run = executor.runs.get(uuid).get();

        if (run.completed) {
          VLOG(1) << "Skipping recovery of executor '" << executor.id
                  << "' of framework " << framework.id
                  << " because its latest run " << uuid << " is completed";
          continue;
        }

        // TODO(vinod): Currently, we assume that the cgroups
        // information (e.g., hierarchy, root) used while recovering
        // is same as the one that was used by the previous slave
        // while checkpointing. Instead, we should checkpoint the
        // cgroups information.
        CgroupInfo* info = registerCgroupInfo(
            framework.id, executor.id, uuid, run.forkedPid, flags);

        // If the cgroup has already been removed inform the slave.
        Try<bool> exists = cgroups::exists(hierarchy, info->name());
        CHECK_SOME(exists) << "Failed to find the existence of cgroup "
                           << info->name();
        if (!exists.get()) {
          dispatch(slave,
                   &Slave::executorTerminated,
                   info->frameworkId,
                   info->executorId,
                   info->status,
                   info->destroyed,
                   info->message);

          unregisterCgroupInfo(framework.id, executor.id);

          continue;
        }

        cgroups.insert(info->name());

        // Add the pid to the reaper to monitor exit status.
        if (run.forkedPid.isSome()) {
          reaper.monitor(run.forkedPid.get())
            .onAny(defer(PID<CgroupsIsolator>(this),
                         &CgroupsIsolator::reaped,
                         run.forkedPid.get(),
                         lambda::_1));
        }

        // Start listening for OOMs. If the executor OOMed while the
        // slave was down or recovering, the cgroup will already be
        // under_oom, resulting in immediate notification.
        // TODO(bmahler): I've been unable to find documentation
        // guaranteeing this, but the kernel source indicates they
        // notify if already under_oom.
        if (subsystems.contains("memory")) {
          oomListen(framework.id, executor.id);
        }
      }
    }
  }

  // Cleanup any orphaned cgroups that are not going to be recovered (this
  // should be safe because we've been able to acquire the file lock).
  Try<vector<string> > orphans = cgroups::get(hierarchy, flags.cgroups_root);
  if (orphans.isError()) {
    return Future<Nothing>::failed(orphans.error());
  }

  foreach (const string& orphan, orphans.get()) {
    if (!cgroups.contains(orphan)) {
      LOG(INFO) << "Removing orphaned cgroup '" << orphan << "'";
      cgroups::destroy(hierarchy, orphan)
        .onAny(defer(PID<CgroupsIsolator>(this),
               &CgroupsIsolator::_destroy,
               orphan,
               lambda::_1));
    }
  }

  return Nothing();
}


void CgroupsIsolator::reaped(pid_t pid, const Future<Option<int> >& status)
{
  CgroupInfo* info = findCgroupInfo(pid);
  if (info != NULL) {
    FrameworkID frameworkId = info->frameworkId;
    ExecutorID executorId = info->executorId;

    if (!status.isReady()) {
      LOG(ERROR) << "Failed to get the status for executor " << executorId
                 << " of framework " << frameworkId << ": "
                 << (status.isFailed() ? status.failure() : "discarded");
      return;
    }

    LOG(INFO) << "Executor " << executorId
              << " of framework " << frameworkId
              << " terminated with status "
              << (status.get().isSome()
                  ? stringify(status.get().get())
                  : "unknown");

    // Set the exit status, so that '_killExecutor()' can send it to the slave.
    info->status = status.get();

    if (!info->killed) {
      killExecutor(frameworkId, executorId);
    }
  }
}


Try<Nothing> CgroupsIsolator::cpusChanged(
    CgroupInfo* info,
    const Resource& resource)
{
  CHECK(resource.name() == "cpus");

  if (resource.type() != Value::SCALAR) {
    return Error("Expecting resource 'cpus' to be a scalar");
  }

  double cpus = resource.scalar().value();
  size_t shares =
    std::max((size_t) (CPU_SHARES_PER_CPU * cpus), MIN_CPU_SHARES);

  Try<Nothing> write = cgroups::write(
      hierarchy, info->name(), "cpu.shares", stringify(shares));

  if (write.isError()) {
    return Error("Failed to update 'cpu.shares': " + write.error());
  }

  LOG(INFO) << "Updated 'cpu.shares' to " << shares
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  return Nothing();
}


Try<Nothing> CgroupsIsolator::cpusetChanged(
    CgroupInfo* info,
    const Resource& resource)
{
  CHECK_NOTNULL(info->cpuset);
  CHECK(resource.name() == "cpus");
  CHECK(resource.type() == Value::SCALAR);

  double delta = resource.scalar().value() - info->cpuset->usage();

  if (delta < 0) {
    map<proc::CPU, double> deallocated = info->cpuset->shrink(fabs(delta));
    foreachpair (const proc::CPU& cpu, double freed, deallocated) {
      cpus[cpu] -= freed;
      CHECK(cpus[cpu] > -0.001); // Check approximately >= 0.
    }
  } else {
    map<proc::CPU, double> allocated = info->cpuset->grow(delta, cpus);
    foreachpair (const proc::CPU& cpu, double used, allocated) {
      cpus[cpu] += used;
      CHECK(cpus[cpu] < 1.001); // Check approximately <= 1.
    }
  }

  Try<Nothing> write = cgroups::write(
      hierarchy, info->name(), "cpuset.cpus", stringify(*(info->cpuset)));

  if (write.isError()) {
    return Error("Failed to update 'cpuset.cpus': " + write.error());
  }

  LOG(INFO) << "Updated 'cpuset.cpus' to " << *(info->cpuset)
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  return Nothing();
}


Try<Nothing> CgroupsIsolator::cfsChanged(
    CgroupInfo* info,
    const Resource& resource)
{
  CHECK(resource.name() == "cpus");
  CHECK(resource.type() == Value::SCALAR);

  Try<Nothing> write = cgroups::write(
      hierarchy,
      info->name(),
      "cpu.cfs_period_us",
      stringify(CPU_CFS_PERIOD.us()));

  if (write.isError()) {
    return Error("Failed to update 'cpu.cfs_period_us': " + write.error());
  }

  double cpus = resource.scalar().value();
  size_t quota = static_cast<size_t>(
    std::max(CPU_CFS_PERIOD.us() * cpus, MIN_CPU_CFS_QUOTA.us()));

  write = cgroups::write(
      hierarchy, info->name(), "cpu.cfs_quota_us", stringify(quota));

  if (write.isError()) {
    return Error("Failed to update 'cpu.cfs_quota_us': " + write.error());
  }

  LOG(INFO) << "Updated 'cpu.cfs_period_us' to " << CPU_CFS_PERIOD.us()
            << " and 'cpu.cfs_quota_us' to " << quota
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  // Set cpu.shares as well.
  // TODO(tdmackey): Allow multiple handlers per resource.
  cpusChanged(info, resource);

  return Nothing();
}


Try<Nothing> CgroupsIsolator::memChanged(
    CgroupInfo* info,
    const Resource& resource)
{
  CHECK(resource.name() == "mem");

  if (resource.type() != Value::SCALAR) {
    return Error("Expecting resource 'mem' to be a scalar");
  }

  Bytes mem = Bytes((uint64_t) resource.scalar().value() * 1024LL * 1024LL);
  Bytes limit = std::max(mem, MIN_MEMORY);

  // Always set the soft limit.
  Try<Nothing> write =
    cgroups::memory::soft_limit_in_bytes(hierarchy, info->name(), limit);

  if (write.isError()) {
    return Error("Failed to set 'memory.soft_limit_in_bytes': "
        + write.error());
  }

  LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to " << limit
            << " for executor " << info->executorId
            << " of framework " << info->frameworkId;

  // Read the existing limit.
  Try<Bytes> currentLimit =
    cgroups::memory::limit_in_bytes(hierarchy, info->name());

  if (currentLimit.isError()) {
    return Error(
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
  // discrepancy between usage and soft limit and introduces a
  // "manual oom" if necessary.
  if (info->pid.isNone() || limit > currentLimit.get()) {
    write = cgroups::memory::limit_in_bytes(hierarchy, info->name(), limit);

    if (write.isError()) {
      return Error("Failed to set 'memory.limit_in_bytes': " + write.error());
    }

    LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << limit
              << " for executor " << info->executorId
              << " of framework " << info->frameworkId;
  }

  return Nothing();
}


void CgroupsIsolator::oomListen(
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

  CHECK_SOME(info->uuid);
  info->oomNotifier.onAny(
      defer(PID<CgroupsIsolator>(this),
            &CgroupsIsolator::oomWaited,
            frameworkId,
            executorId,
            info->uuid.get(),
            lambda::_1));
}


void CgroupsIsolator::oomWaited(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& uuid,
    const Future<uint64_t>& future)
{
  LOG(INFO) << "OOM notifier is triggered for executor "
            << executorId << " of framework " << frameworkId
            << " with uuid " << uuid;

  if (future.isDiscarded()) {
    LOG(INFO) << "Discarded OOM notifier for executor "
              << executorId << " of framework " << frameworkId
              << " with uuid " << uuid;
  } else if (future.isFailed()) {
    LOG(ERROR) << "Listening on OOM events failed for executor "
               << executorId << " of framework " << frameworkId
               << " with uuid " << uuid << ": " << future.failure();
  } else {
    // Out-of-memory event happened, call the handler.
    oom(frameworkId, executorId, uuid);
  }
}


void CgroupsIsolator::oom(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& uuid)
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
  CHECK_SOME(info->uuid);
  if (uuid != info->uuid.get()) {
    LOG(INFO) << "OOM detected for a previous executor instance";
    return;
  }

  // If killed is set, the OOM notifier will be discarded in oomWaited.
  // Therefore, we should not be able to reach this point.
  CHECK(!info->killed) << "OOM detected for an already killed executor";

  LOG(INFO) << "OOM detected for executor " << executorId
            << " of framework " << frameworkId
            << " with uuid " << uuid;

  // Construct a "message" string to describe why the isolator
  // destroyed the executor's cgroup (in order to assist in debugging).
  ostringstream message;
  message << "Memory limit exceeded: ";

  // Output the requested memory limit.
  Try<Bytes> limit = cgroups::memory::limit_in_bytes(hierarchy, info->name());

  if (limit.isError()) {
    LOG(ERROR) << "Failed to read 'memory.limit_in_bytes': " << limit.error();
  } else {
    message << "Requested: " << limit.get() << " ";
  }

  // Output the memory usage.
  Try<Bytes> usage = cgroups::memory::usage_in_bytes(hierarchy, info->name());

  if (usage.isError()) {
    LOG(ERROR) << "Failed to read 'memory.usage_in_bytes': " << usage.error();
  } else {
    message << "Used: " << usage.get() << "\n";
  }

  // Output 'memory.stat' of the cgroup to help with debugging.
  Try<string> read = cgroups::read(hierarchy, info->name(), "memory.stat");
  if (read.isError()) {
    LOG(ERROR) << "Failed to read 'memory.stat': " << read.error();
  } else {
    message << "\nMEMORY STATISTICS: \n" << read.get() << "\n";
  }

  LOG(INFO) << strings::trim(message.str()); // Trim the extra '\n' at the end.

  info->destroyed = true;
  info->message = message.str();

  killExecutor(frameworkId, executorId);
}


void CgroupsIsolator::_destroy(
    const string& cgroup,
    const Future<bool>& future)
{
  CHECK(initialized) << "Cannot destroy cgroups before initialization";

  if (future.isReady()) {
    LOG(INFO) << "Successfully destroyed cgroup " << cgroup;
  } else {
    LOG(FATAL) << "Failed to destroy cgroup " << cgroup
               << ": " << future.failure();
  }
}


void CgroupsIsolator::_killExecutor(
    CgroupInfo* info,
    const Future<bool>& future)
{
  CHECK(initialized) << "Cannot kill executors before initialization";

  CHECK_NOTNULL(info);

  if (future.isReady()) {
    LOG(INFO) << "Successfully destroyed cgroup " << info->name();

    CHECK(info->killed)
      << "Unexpectedly alive executor " << info->executorId
      << " of framework " << info->frameworkId;

    // NOTE: The exit status of the executor might not be set if this
    // function is called before 'processTerminated()' is called.
    // TODO(vinod): When reaper returns a future instead of issuing a callback,
    // wait for that future to be ready and grab the exit status.
    dispatch(slave,
             &Slave::executorTerminated,
             info->frameworkId,
             info->executorId,
             info->status,
             info->destroyed,
             info->message);

    // We make a copy here because 'info' will be deleted when we unregister.
    unregisterCgroupInfo(
        utils::copy(info->frameworkId),
        utils::copy(info->executorId));
  } else {
    LOG(FATAL) << "Failed to destroy cgroup " << info->name()
               << ": " << future.failure();
  }
}


CgroupsIsolator::CgroupInfo* CgroupsIsolator::registerCgroupInfo(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& uuid,
    const Option<pid_t>& pid,
    const Flags& flags)
{
  CgroupInfo* info = new CgroupInfo();
  info->frameworkId = frameworkId;
  info->executorId = executorId;
  info->uuid = uuid;
  info->pid = pid;
  info->killed = false;
  info->destroyed = false;
  info->status = -1;
  info->message = "";
  info->flags = flags;
  if (subsystems.contains("cpuset")) {
    info->cpuset = new Cpuset();
  } else {
    info->cpuset = NULL;
  }
  infos[frameworkId][executorId] = info;
  return info;
}


void CgroupsIsolator::unregisterCgroupInfo(
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


CgroupsIsolator::CgroupInfo* CgroupsIsolator::findCgroupInfo(
    pid_t pid)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachvalue (CgroupInfo* info, infos[frameworkId]) {
      if (info->pid.isSome() && info->pid.get() == pid) {
        return info;
      }
    }
  }
  return NULL;
}


CgroupsIsolator::CgroupInfo* CgroupsIsolator::findCgroupInfo(
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
