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

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/utils.hpp>

#include "slave/reaper.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

ReaperProcess::ReaperProcess()
  : ProcessBase(ID::generate("reaper")) {}


Future<int> ReaperProcess::monitor(pid_t pid)
{
  // Check to see if the current process has sufficient privileges to
  // monitor the liveness of this pid.
  Try<bool> alive = os::alive(pid);

  if (alive.isSome()) {
    if (alive.get()) {
      // We have permissions to check the validity of the process
      // and it's alive, so add it to the promises map.
      Owned<Promise<int> > promise(new Promise<int>());
      promises.put(pid, promise);
      return promise->future();
    } else {
      // Process doesn't exist.
      LOG(WARNING) << "Cannot monitor process " << pid
                   << " because it doesn't exist";
      return -1;
    }
  }

  // Now we know we don't have permission for alive(), but we can
  // still monitor it if it is our child.
  int status;
  pid_t result = waitpid(pid, &status, WNOHANG);

  if (result > 0) {
    // The process terminated and the status was reaped.
    // Notify other listeners and return directly for this caller.
    notify(pid, status);
    return status;
  } else if (result == 0) {
    // Child still active, add to the map.
    Owned<Promise<int> > promise(new Promise<int>());
    promises.put(pid, promise);
    return promise->future();
  } else {
    // Not a child nor do we have permission to for os::alive();
    // we cannot monitor this pid.
    return Future<int>::failed("Failed to monitor process " +
                               stringify(pid) + ": " + strerror(errno));
  }
}


void ReaperProcess::initialize()
{
  reap();
}


void ReaperProcess::notify(pid_t pid, int status)
{
  foreach (const Owned<Promise<int> >& promise, promises.get(pid)) {
    promise->set(status);
  }
  promises.remove(pid);
}


void ReaperProcess::reap()
{
  // This method assumes that the registered PIDs are
  // 1) children, or
  // 2) non-children that we have permission to check liveness, or
  // 3) nonexistent / reaped elsewhere.

  // Reap all child processes first.
  pid_t pid;
  int status;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // Ignore this if the process has only stopped.
    // Notify the "listeners" only if they have requested to monitor
    // this pid. Otherwise the status is discarded.
    // This means if a child pid is registered via the monitor() call
    // after it's reaped, an invalid status (-1) will be returned.
    if (!WIFSTOPPED(status) && promises.contains(pid)) {
      notify(pid, status);
    }
  }

  // Check whether any monitored process has exited and been reaped.
  // 1) If a child terminates before the foreach loop but after the
  //    while loop, it won't be reaped until the next reap() cycle
  //    and the alive() check below returns true.
  // 2) If a non-child process terminates and is reaped elsewhere,
  //    e.g. by init, we notify the listeners. (We know we have
  //    permission to check its liveness in this case.)
  // 3) If a non-child process terminates and is not yet reaped,
  //    alive() returns true and no notification is sent.
  // 4) If a child terminates before the while loop above, then we've
  //    already reaped it and have the listeners notified!
  foreach (pid_t pid, utils::copy(promises.keys())) {
    Try<bool> alive = os::alive(pid);

    if (alive.isSome() && !alive.get()) {
      // The process has been reaped.
      LOG(WARNING) << "Cannot get the exit status of process " << pid
                   << " because it is not a child of the calling "
                   << "process: " << strerror(errno);
      notify(pid, -1);
    }
  }

  delay(Seconds(1), self(), &ReaperProcess::reap); // Reap forever!
}


Reaper::Reaper()
{
  process = new ReaperProcess();
  spawn(process);
}


Reaper::~Reaper()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<int> Reaper::monitor(pid_t pid)
{
  return dispatch(process, &ReaperProcess::monitor, pid);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


