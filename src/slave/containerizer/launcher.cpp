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

#include <unistd.h>

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/process.hpp>
#include <process/reap.hpp>

#include <stout/unreachable.hpp>

#include "mesos/resources.hpp"

#include "slave/containerizer/launcher.hpp"

using namespace process;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using state::RunState;

Try<Launcher*> PosixLauncher::create(const Flags& flags)
{
  return new PosixLauncher();
}


Try<Nothing> PosixLauncher::recover(const list<RunState>& states)
{
  foreach (const RunState& state, states) {
    if (state.id.isNone()) {
      return Error("ContainerID is required to recover");
    }

    const ContainerID& containerId = state.id.get();

    if (state.forkedPid.isNone()) {
      return Error("Executor pid is required to recover container " +
                   stringify(containerId));
    }
    pid_t pid = state.forkedPid.get();

    if (pids.containsValue(pid)) {
      // This should (almost) never occur. There is the possibility that a new
      // executor is launched with the same pid as one that just exited (highly
      // unlikely) and the slave dies after the new executor is launched but
      // before it hears about the termination of the earlier executor (also
      // unlikely). Regardless, the launcher can't do anything sensible so this
      // is considered an error.
      return Error("Detected duplicate pid " + stringify(pid) +
                   " for container " + stringify(containerId));
    }

    pids.put(containerId, pid);
  }

  return Nothing();
}


Try<pid_t> PosixLauncher::fork(
    const ContainerID& containerId,
    const lambda::function<int()>& inChild)
{
  if (pids.contains(containerId)) {
    return Error("Process has already been forked for container " +
                 stringify(containerId));
  }

  pid_t pid;

  if ((pid = ::fork()) == -1) {
    return ErrnoError("Failed to fork");
  }

  if (pid == 0) {
    // In child.
    // POSIX guarantees a forked child's pid does not match any existing
    // process group id so only a single setsid() is required and the session
    // id will be the pid.
    // TODO(idownes): perror is not listed as async-signal-safe and should be
    // reimplemented safely.
    if (setsid() == -1) {
      perror("Failed to put child in a new session");
      _exit(1);
    }

    // This function should exec() and therefore not return.
    inChild();

    const char* message = "Child failed to exec";
    while (write(STDERR_FILENO, message, strlen(message)) == -1 &&
        errno == EINTR);

    _exit(1);
  }

  // parent.
  LOG(INFO) << "Forked child with pid '" << pid
            << "' for container '" << containerId << "'";
  // Store the pid (session id and process group id).
  pids.put(containerId, pid);

  return pid;
}


Future<Nothing> _destroy(const Future<Option<int> >& future)
{
  if (future.isReady()) {
    return Nothing();
  } else {
    return Failure("Failed to kill all processes: " +
                   (future.isFailed() ? future.failure() : "unknown error"));
  }
}


Future<Nothing> PosixLauncher::destroy(const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    return Failure("Unknown container " + containerId.value());
  }

  pid_t pid = pids.get(containerId).get();

  // Kill all processes in the session and process group.
  Try<list<os::ProcessTree> > trees =
    os::killtree(pid, SIGKILL, true, true);

  pids.erase(containerId);

  // The child process may not have been waited on yet so we'll delay
  // completing destroy until we're sure it has been reaped.
  return process::reap(pid)
    .then(lambda::bind(&_destroy, lambda::_1));
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
