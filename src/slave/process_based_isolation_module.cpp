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
#include <string.h>

#include <map>

#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"
#include "common/process_utils.hpp"

#include "slave/flags.hpp"
#include "slave/process_based_isolation_module.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using launcher::ExecutorLauncher;

using std::map;
using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : ProcessBase(ID::generate("process-isolation-module")),
    initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addProcessExitedListener, this);
}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  wait(reaper);
  delete reaper;
}


void ProcessBasedIsolationModule::initialize(
    const Flags& _flags,
    bool _local,
    const PID<Slave>& _slave)
{
  flags = _flags;
  local = _local;
  slave = _slave;

  initialized = true;
}


void ProcessBasedIsolationModule::launchExecutor(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
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

  // Store the working directory, so that in the future we can use it
  // to retrieve the os pid when calling killtree on the executor.
  ProcessInfo* info = new ProcessInfo();
  info->frameworkId = frameworkId;
  info->executorId = executorId;
  info->directory = directory;
  info->pid = -1; // Initialize this variable to handle corner cases.

  infos[frameworkId][executorId] = info;

  // Use pipes to determine which child has successfully changed session.
  int pipes[2];
  if (pipe(pipes) < 0) {
    PLOG(FATAL) << "Failed to create pipe: " << strerror(errno);
  }

  // Set the FD_CLOEXEC flags on these pipes
  Try<bool> result = os::cloexec(pipes[0]);
  CHECK(result.isSome()) << "Error setting FD_CLOEXEC on pipe[0] "
                         << result.error();

  result = os::cloexec(pipes[1]);
  CHECK(result.isSome()) << "Error setting FD_CLOEXEC on pipe[1] "
                         << result.error();

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      PLOG(FATAL) << "Failed to get child PID from pipe";
    }

    close(pipes[0]);

    // In parent process.
    LOG(INFO) << "Forked executor at " << pid;

    // Record the pid (should also be the pgis since we setsid below).
    infos[frameworkId][executorId]->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted,
             frameworkId, executorId, pid);
  } else {
    // In child process, make cleanup easier.
    // NOTE: We setsid() in a loop because setsid() might fail if another
    // process has the same process group id as the calling process.
    close(pipes[0]);
    while ((pid = setsid()) == -1) {
      PLOG(ERROR) << "Could not put executor in own session, "
                  << "forking another process and retrying";

      if ((pid = fork()) == -1) {
        LOG(ERROR) << "Failed to fork to launch executor";
        exit(-1);
      }

      if (pid) {
        // In parent process.
        // It is ok to suicide here, though process reaper signals the exit,
        // because the process isolation module ignores unknown processes.
        exit(-1);
      }
    }

    if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
      PLOG(FATAL) << "Failed to write PID on pipe";
    }

    close(pipes[1]);

    ExecutorLauncher* launcher =
      createExecutorLauncher(frameworkId, frameworkInfo,
                             executorInfo, directory);

    launcher->run();
  }
}

// NOTE: This function can be called by the isolation module itself or
// by the slave if it doesn't hear about an executor exit after it sends
// a shutdown message.
void ProcessBasedIsolationModule::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(initialized) << "Cannot kill executors before initialization!";
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId)) {
    LOG(ERROR) << "ERROR! Asked to kill an unknown executor! " << executorId;
    return;
  }

  pid_t pid = infos[frameworkId][executorId]->pid;

  if (pid != -1) {
    // TODO(vinod): Call killtree on the pid of the actual executor process
    // that is running the tasks (stored in the local storage by the
    // executor module).
    utils::process::killtree(pid, SIGKILL, true, true, true);

    // Also kill all processes that belong to the process group of the executor.
    // This is valuable in situations where the top level executor process
    // exited and hence killtree is unable to kill any spawned orphans.
    // NOTE: This assumes that the process group id of the executor process is
    // same as its pid (which is expected to be the case with setsid()).
    // TODO(vinod): Also (recursively) kill processes belonging to the
    // same session, but have a different process group id.
    if (killpg(pid, SIGKILL) == -1 && errno != ESRCH) {
      LOG(ERROR) << "ERROR! Killing process group " << pid;
    }

    ProcessInfo* info = infos[frameworkId][executorId];

    if (infos[frameworkId].size() == 1) {
      infos.erase(frameworkId);
    } else {
      infos[frameworkId].erase(executorId);
    }

    delete info;
  }
}


void ProcessBasedIsolationModule::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot do resourcesChanged before initialization!";
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory)
{
  return new ExecutorLauncher(frameworkId,
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
}


void ProcessBasedIsolationModule::processExited(pid_t pid, int status)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachpair (
        const ExecutorID& executorId, ProcessInfo* info, infos[frameworkId]) {
      if (info->pid == pid) {
        LOG(INFO) << "Telling slave of lost executor " << executorId
                  << " of framework " << frameworkId;

        dispatch(slave, &Slave::executorExited,
                 frameworkId, executorId, status);

        // Try and cleanup after the executor.
        killExecutor(frameworkId, executorId);
        return;
      }
    }
  }
}
