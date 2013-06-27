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


Future<Option<int> > ReaperProcess::monitor(pid_t pid)
{
  // Check to see if this pid exists.
  const Result<os::Process>& process = os::process(pid);

  if (process.isSome()) {
    // The process exists, we add it to the promises map.
    Owned<Promise<Option<int> > > promise(new Promise<Option<int> >());
    promises.put(pid, promise);
    return promise->future();
  } else if (process.isNone()) {
    LOG(WARNING) << "Cannot monitor process " << pid
                 << " because it doesn't exist";
    return None();
  } else {
    return Future<Option<int> >::failed(
        "Failed to monitor process " + stringify(pid) + ": " + process.error());
  }
}


void ReaperProcess::initialize()
{
  reap();
}


void ReaperProcess::notify(pid_t pid, Option<int> status)
{
  foreach (const Owned<Promise<Option<int> > >& promise, promises.get(pid)) {
    promise->set(status);
  }
  promises.remove(pid);
}


void ReaperProcess::reap()
{
  // This method assumes that the registered PIDs are
  // 1) children: we can reap their exit status when they are
  //    terminated.
  // 2) non-children: we cannot reap their exit status.
  // 3) nonexistent: already reaped elsewhere.

  // Reap all child processes first.
  pid_t pid;
  int status;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // Ignore this if the process has only stopped.
    // Notify the "listeners" only if they have requested to monitor
    // this pid. Otherwise the status is discarded.
    // This means if a child pid is registered via the monitor() call
    // after it's reaped, status 'None' will be returned.
    if (!WIFSTOPPED(status) && promises.contains(pid)) {
      notify(pid, status);
    }
  }

  // Check whether any monitored process has exited and been reaped.
  // 1) If a child terminates before the foreach loop but after the
  //    while loop, it won't be reaped until the next reap() cycle.
  // 2) If a non-child process terminates and is reaped elsewhere,
  //    e.g. by init, we notify the listeners.
  // 3) If a non-child process terminates and is not yet reaped,
  //    no notification is sent.
  // 4) If a child terminates before the while loop above, then we've
  //    already reaped it and have the listeners notified!
  foreach (pid_t pid, utils::copy(promises.keys())) {
    const Result<os::Process>& process = os::process(pid);

    if (process.isError()) {
      LOG(ERROR) << "Failed to get process information for " << pid
                 <<": " << process.error();
      notify(pid, None());
    } else if (process.isNone()) {
      // The process has been reaped.
      LOG(WARNING) << "Cannot get the exit status of process " << pid
                   << " because it no longer exists";
      notify(pid, None());
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


Future<Option<int> > Reaper::monitor(pid_t pid)
{
  return dispatch(process, &ReaperProcess::monitor, pid);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


