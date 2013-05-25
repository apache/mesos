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

#include <glog/logging.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/utils.hpp>

#include "logging/check_some.hpp"

#include "slave/reaper.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

Reaper::Reaper()
  : ProcessBase(ID::generate("reaper")) {}


Reaper::~Reaper() {}


void Reaper::addListener(
    const PID<ProcessExitedListener>& listener)
{
  listeners.push_back(listener);
}


Try<Nothing> Reaper::monitor(pid_t pid)
{
  // Check to see if the current process has sufficient privileges to
  // monitor the liveness of this pid.
  Try<bool> alive = os::alive(pid);
  if (alive.isError()) {
    return Error("Failed to monitor process " + stringify(pid) +
                  ": " + alive.error());
  } else {
    pids.insert(pid);
  }
  return Nothing();
}


void Reaper::initialize()
{
  reap();
}


void Reaper::notify(pid_t pid, int status)
{
  foreach (const PID<ProcessExitedListener>& listener, listeners) {
    dispatch(listener, &ProcessExitedListener::processExited, pid, status);
  }
}


void Reaper::reap()
{
  // Check whether any monitored process has exited.
  foreach (pid_t pid, utils::copy(pids)) {
    Try<bool> alive = os::alive(pid);
    CHECK_SOME(alive);

    if (!alive.get()) { // The process has terminated.
      // Attempt to reap the status.
      // If pid is not a child process of the current process, this is a no-op.
      int status = -1;
      if (waitpid(pid, &status, WNOHANG) < 0) {
        LOG(WARNING) << "Cannot get the exit status of process " << pid
                     << " because it either does not exist or"
                     << " is not a child of the calling process: "
                     << strerror(errno);
      }

      notify(pid, status); // Notify the listeners.
      pids.erase(pid);
    }
  }

  // Check whether any child processes have exited.
  pid_t pid;
  int status;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // Ignore this if the process has only stopped.
    if (!WIFSTOPPED(status)) {
      notify(pid, status); // Notify the listeners.
      pids.erase(pid);
    }
  }
  delay(Seconds(1), self(), &Reaper::reap); // Reap forever!
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


