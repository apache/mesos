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

#include <list>
#include <map>
#include <set>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "slave/flags.hpp"
#include "slave/process_isolator.hpp"
#include "slave/state.hpp"

using namespace process;

using std::map;
using std::set;
using std::string;

using process::defer;
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
    initialized(false) {}


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
      frameworkInfo.checkpoint(),
      flags.recovery_timeout);

  // We get the environment map for launching mesos-launcher before
  // the fork, because we have seen deadlock issues with ostringstream
  // in the forked process before it calls exec.
  map<string, string> env = launcher.getLauncherEnvironment();

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid > 0) {
    os::close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      PLOG(FATAL) << "Failed to get child PID from pipe";
    }

    os::close(pipes[0]);

    // In parent process.
    LOG(INFO) << "Forked executor at " << pid;

    // Record the pid (should also be the pgid since we setsid below).
    infos[frameworkId][executorId]->pid = pid;

    reaper.monitor(pid)
      .onAny(defer(PID<ProcessIsolator>(this),
                   &ProcessIsolator::reaped,
                   pid,
                   lambda::_1));

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted, frameworkId, executorId, pid);
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
              << "for the mesos-launcher '"
              << path::join(flags.launcher_dir, "mesos-launcher")
              << "': " << realpath.error();
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
    Try<std::list<os::ProcessTree> > trees =
      os::killtree(pid.get(), SIGKILL, true, true);

    if (trees.isError()) {
      LOG(WARNING) << "Failed to kill the process tree rooted at pid "
                   << pid.get() << ": " << trees.error();
    } else {
      LOG(INFO) << "Killed the following process trees:\n"
                << stringify(trees.get());
    }

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

  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId) ||
      infos[frameworkId][executorId]->killed) {
    LOG(INFO) << "Asked to update resources for an unknown/killed executor '"
              << executorId << "' of framework " << frameworkId;
    return;
  }

  ProcessInfo* info = CHECK_NOTNULL(infos[frameworkId][executorId]);

  info->resources = resources;

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

      if (run.completed) {
        VLOG(1) << "Skipping recovery of executor '" << executor.id
                << "' of framework " << framework.id
                << " because its latest run " << uuid << " is completed";
        continue;
      }

      ProcessInfo* info =
        new ProcessInfo(framework.id, executor.id, run.forkedPid);

      infos[framework.id][executor.id] = info;

      // Add the pid to the reaper to monitor exit status.
      if (run.forkedPid.isSome()) {
        reaper.monitor(run.forkedPid.get())
          .onAny(defer(PID<ProcessIsolator>(this),
                       &ProcessIsolator::reaped,
                       run.forkedPid.get(),
                       lambda::_1));
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
  CHECK_NOTNULL(info);

  ResourceStatistics result;

  result.set_timestamp(Clock::now().secs());

  // Set the resource allocations.
  const Option<Bytes>& mem = info->resources.mem();
  if (mem.isSome()) {
    result.set_mem_limit_bytes(mem.get().bytes());
  }

  const Option<double>& cpus = info->resources.cpus();
  if (cpus.isSome()) {
    result.set_cpus_limit(cpus.get());
  }

  CHECK_SOME(info->pid);

  Result<os::Process> process = os::process(info->pid.get());

  if (!process.isSome()) {
    return Future<ResourceStatistics>::failed(
        process.isError() ? process.error() : "Process does not exist");
  }

  result.set_timestamp(Clock::now().secs());

  if (process.get().rss.isSome()) {
    result.set_mem_rss_bytes(process.get().rss.get().bytes());
  }

  // We only show utime and stime when both are available, otherwise
  // we're exposing a partial view of the CPU times.
  if (process.get().utime.isSome() && process.get().stime.isSome()) {
    result.set_cpus_user_time_secs(process.get().utime.get().secs());
    result.set_cpus_system_time_secs(process.get().stime.get().secs());
  }

  // Now aggregate all descendant process usage statistics.
  const Try<set<pid_t> >& children = os::children(info->pid.get(), true);

  if (children.isError()) {
    return Future<ResourceStatistics>::failed(
        "Failed to get children of " + stringify(info->pid.get()) + ": " +
        children.error());
  }

  // Aggregate the usage of all child processes.
  foreach (pid_t child, children.get()) {
    process = os::process(child);

    // Skip processes that disappear.
    if (process.isNone()) {
      continue;
    }

    if (process.isError()) {
      LOG(WARNING) << "Failed to get status of descendant process " << child
                   << " of parent " << info->pid.get() << ": "
                   << process.error();
      continue;
    }

    if (process.get().rss.isSome()) {
      result.set_mem_rss_bytes(
          result.mem_rss_bytes() + process.get().rss.get().bytes());
    }

    // We only show utime and stime when both are available, otherwise
    // we're exposing a partial view of the CPU times.
    if (process.get().utime.isSome() && process.get().stime.isSome()) {
      result.set_cpus_user_time_secs(
          result.cpus_user_time_secs() + process.get().utime.get().secs());
      result.set_cpus_system_time_secs(
          result.cpus_system_time_secs() + process.get().stime.get().secs());
    }
  }

  return result;
}


void ProcessIsolator::reaped(pid_t pid, const Future<Option<int> >& status)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachkey (const ExecutorID& executorId, infos[frameworkId]) {
      ProcessInfo* info = infos[frameworkId][executorId];

      if (info->pid.isSome() && info->pid.get() == pid) {
        if (!status.isReady()) {
          LOG(ERROR) << "Failed to get the status for executor '" << executorId
                     << "' of framework " << frameworkId << ": "
                     << (status.isFailed() ? status.failure() : "discarded");
          return;
        }

        LOG(INFO) << "Telling slave of terminated executor '" << executorId
                  << "' of framework " << frameworkId;

        dispatch(slave,
                 &Slave::executorTerminated,
                 frameworkId,
                 executorId,
                 status.get(),
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
