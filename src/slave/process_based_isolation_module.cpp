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

#include <map>

#include <process/dispatch.hpp>

#include "process_based_isolation_module.hpp"

#include "common/foreach.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"
#include "common/process_utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using launcher::ExecutorLauncher;

using std::map;
using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : initialized(false)
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
    const Configuration& _conf,
    bool _local,
    const PID<Slave>& _slave)
{
  conf = _conf;
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
            << " (" << executorInfo.uri() << ")"
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

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    // In parent process.
    LOG(INFO) << "Forked executor at " << pid;

    // Record the pid (should also be the pgis since we setsid below).
    infos[frameworkId][executorId]->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted,
             frameworkId, executorId, pid);
  } else {
    // In child process, make cleanup easier.
    if ((pid = setsid()) == -1) {
      PLOG(FATAL) << "Failed to put executor in own session";
    }

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
    utils::process::killtree(pid, SIGKILL, true, true);

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
                              executorInfo.uri(),
                              frameworkInfo.user(),
                              directory,
                              slave,
                              conf.get("frameworks_home", ""),
                              conf.get("home", ""),
                              conf.get("hadoop_home", ""),
                              !local,
                              conf.get("switch_user", true),
                              "",
                              executorInfo.environment());
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
