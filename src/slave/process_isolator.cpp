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

#include <errno.h>
#include <signal.h>
#include <stdio.h> // For perror.
#include <string.h>

#ifdef __APPLE__
#include <libproc.h> // For proc_pidinfo.
#endif

#include <map>
#include <set>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#ifdef __linux__
#include "stout/proc.hpp"
#endif
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"
#include "common/process_utils.hpp"

#include "slave/flags.hpp"
#include "slave/process_isolator.hpp"
#include "slave/state.hpp"

using namespace process;

using std::map;
using std::set;
using std::string;

using process::wait; // Necessary on some OS's to disambiguate.

namespace mesos {
namespace internal {
namespace slave {

using launcher::ExecutorLauncher;

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;

ProcessIsolator::ProcessIsolator()
  : ProcessBase(ID::generate("process-isolator")),
    initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addListener, this);
}


ProcessIsolator::~ProcessIsolator()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  wait(reaper);
  delete reaper;
}


void ProcessIsolator::initialize(
    const Flags& _flags,
    const Resources& _,
    bool _local,
    const PID<Slave>& _slave)
{
  flags = _flags;
  local = _local;
  slave = _slave;

  initialized = true;
}


void ProcessIsolator::launchExecutor(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const UUID& uuid,
    const string& directory,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot launch executors before initialization!";

  const ExecutorID& executorId = executorInfo.executor_id();

  LOG(INFO) << "Launching " << executorId
            << " (" << executorInfo.command().value() << ")"
            << " in " << directory
            << " with resources " << resources
            << "' for framework " << frameworkId;

  ProcessInfo* info = new ProcessInfo(frameworkId, executorId);

  infos[frameworkId][executorId] = info;

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

  // Create the ExecutorLauncher instance before the fork for the
  // child process to use.
  ExecutorLauncher launcher(
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
      frameworkInfo.checkpoint());

  // We get the environment map for launching mesos-launcher before
  // the fork, because we have seen deadlock issues with ostringstream
  // in the forked process before it calls exec.
  map<string, string> env = launcher.getLauncherEnvironment();

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid > 0) {
    close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      PLOG(FATAL) << "Failed to get child PID from pipe";
    }

    close(pipes[0]);

    // In parent process.
    LOG(INFO) << "Forked executor at " << pid;

    // Record the pid (should also be the pgid since we setsid below).
    infos[frameworkId][executorId]->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted, frameworkId, executorId, pid);
  } else {
    // In child process, we make cleanup easier by putting process
    // into it's own session. DO NOT USE GLOG!
    close(pipes[0]);

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
        // It is ok to suicide here, though process reaper signals the exit,
        // because the process isolator ignores unknown processes.
        exit(0);
      }
    }

    if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
      perror("Failed to write PID on pipe");
      abort();
    }

    close(pipes[1]);

    // Setup the environment for launcher.
    foreachpair (const string& key, const string& value, env) {
      os::setenv(key, value);
    }

    const char** args = (const char**) new char*[2];

    // Determine path for mesos-launcher.
    Try<string> realpath = os::realpath(
        path::join(flags.launcher_dir, "mesos-launcher"));

    if (realpath.isError()) {
      EXIT(1) << "Failed to determine the canonical path "
              << "for the mesos-launcher: " << realpath.error();
    }

    // Grab a copy of the path so that we can reliably use 'c_str()'.
    const string& path = realpath.get();

    args[0] = path.c_str();
    args[1] = NULL;

    // Execute the mesos-launcher!
    execvp(args[0], (char* const*) args);

    // If we get here, the execvp call failed.
    perror("Failed to execvp the mesos-launcher");
    abort();
  }
}

// NOTE: This function can be called by the isolator itself or by the
// slave if it doesn't hear about an executor exit after it sends a
// shutdown message.
void ProcessIsolator::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(initialized) << "Cannot kill executors before initialization!";

  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId) ||
      infos[frameworkId][executorId]->killed) {
    LOG(ERROR) << "Asked to kill an unknown/killed executor! " << executorId;
    return;
  }

  const Option<pid_t>& pid = infos[frameworkId][executorId]->pid;

  if (pid.isSome()) {
    // TODO(vinod): Call killtree on the pid of the actual executor process
    // that is running the tasks (stored in the local storage by the
    // executor module).
    utils::process::killtree(pid.get(), SIGKILL, true, true, true);

    // Also kill all processes that belong to the process group of the executor.
    // This is valuable in situations where the top level executor process
    // exited and hence killtree is unable to kill any spawned orphans.
    // NOTE: This assumes that the process group id of the executor process is
    // same as its pid (which is expected to be the case with setsid()).
    // TODO(vinod): Also (recursively) kill processes belonging to the
    // same session, but have a different process group id.
    if (killpg(pid.get(), SIGKILL) == -1 && errno != ESRCH) {
      PLOG(WARNING) << "Failed to kill process group " << pid.get();
    }

    infos[frameworkId][executorId]->killed = true;
  }
}


void ProcessIsolator::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot do resourcesChanged before initialization!";
  // Do nothing; subclasses may override this.
}


Future<Nothing> ProcessIsolator::recover(
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering isolator";

  if (state.isNone()) {
    return Nothing();
  }

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
      const RunState& run  = executor.runs.get(uuid).get();

      ProcessInfo* info =
        new ProcessInfo(framework.id, executor.id, run.forkedPid);

      infos[framework.id][executor.id] = info;

      // Add the pid to the reaper to monitor exit status.
      if (run.forkedPid.isSome()) {
        dispatch(reaper, &Reaper::monitor, run.forkedPid.get());
      }
    }
  }

  return Nothing();
}


Future<ResourceStatistics> ProcessIsolator::usage(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId) ||
      infos[frameworkId][executorId]->killed) {
    return Future<ResourceStatistics>::failed("Unknown/killed executor");
  }

  ProcessInfo* info = infos[frameworkId][executorId];

  ResourceStatistics result;

  result.set_timestamp(Clock::now());

#ifdef __linux__
  // Get the page size, used for memory accounting.
  // NOTE: This is more portable than using getpagesize().
  long pageSize = sysconf(_SC_PAGESIZE);
  PCHECK(pageSize > 0) << "Failed to get sysconf(_SC_PAGESIZE)";

  // Get the number of clock ticks, used for cpu accounting.
  long ticks = sysconf(_SC_CLK_TCK);
  PCHECK(ticks > 0) << "Failed to get sysconf(_SC_CLK_TCK)";

  CHECK_SOME(info->pid);

  // Get the parent process usage statistics.
  Try<proc::ProcessStatus> status = proc::status(info->pid.get());

  if (status.isError()) {
    return Future<ResourceStatistics>::failed(status.error());
  }

  result.set_memory_rss(status.get().rss * pageSize);
  result.set_cpu_user_time((double) status.get().utime / (double) ticks);
  result.set_cpu_system_time((double) status.get().stime / (double) ticks);

  // Now aggregate all descendant process usage statistics.
  Try<set<pid_t> > children = proc::children(info->pid.get(), true);

  if (children.isError()) {
    return Future<ResourceStatistics>::failed(
        "Failed to get children of " + stringify(info->pid.get()) + ": " +
        children.error());
  }

  // Aggregate the usage of all child processes.
  foreach (pid_t child, children.get()) {
    status = proc::status(child);

    if (status.isError()) {
      LOG(WARNING) << "Failed to get status of descendant process " << child
                   << " of parent " << info->pid.get() << ": "
                   << status.error();
      continue;
    }

    result.set_memory_rss(
        result.memory_rss() +
        status.get().rss * pageSize);

    result.set_cpu_user_time(
        result.cpu_user_time() +
        (double) status.get().utime / (double) ticks);

    result.set_cpu_system_time(
        result.cpu_system_time() +
        (double) status.get().stime / (double) ticks);
  }
#elif defined __APPLE__
  // TODO(bmahler): Aggregate the usage of all child processes.
  // NOTE: There are several pitfalls to using proc_pidinfo().
  // In particular:
  //   -This will not work for many root processes.
  //   -This may not work for processes owned by other users.
  //   -However, this always works for processes owned by the same user.
  // This beats using task_for_pid(), which only works for the same pid.
  // For further discussion around these issues,
  // see: http://code.google.com/p/psutil/issues/detail?id=297
  CHECK_SOME(info->pid);

  proc_taskinfo task;
  int size =
    proc_pidinfo(info->pid.get(), PROC_PIDTASKINFO, 0, &task, sizeof(task));

  if (size != sizeof(task)) {
    return Future<ResourceStatistics>::failed(
        "Failed to get proc_pidinfo: " + stringify(size));
  }

  result.set_memory_rss(task.pti_resident_size);

  // NOTE: CPU Times are in nanoseconds, but this is not documented!
  result.set_cpu_user_time(Nanoseconds(task.pti_total_user).secs());
  result.set_cpu_system_time(Nanoseconds(task.pti_total_system).secs());
#endif

  return result;
}


void ProcessIsolator::processExited(pid_t pid, int status)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachkey (const ExecutorID& executorId, infos[frameworkId]) {
      ProcessInfo* info = infos[frameworkId][executorId];

      if (info->pid.isSome() && info->pid.get() == pid) {
        LOG(INFO) << "Telling slave of terminated executor '" << executorId
                  << "' of framework " << frameworkId;

        dispatch(slave,
                 &Slave::executorTerminated,
                 frameworkId,
                 executorId,
                 status,
                 false,
                 "Executor terminated");

        if (!info->killed) {
          // Try and cleanup after the executor.
          killExecutor(frameworkId, executorId);
        }

        if (infos[frameworkId].size() == 1) {
          infos.erase(frameworkId);
        } else {
          infos[frameworkId].erase(executorId);
        }
        delete info;

        return;
      }
    }
  }
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
