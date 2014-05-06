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

#include <sched.h>
#include <unistd.h>

#include <linux/sched.h>

#include <vector>

#include <stout/abort.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>

#include "linux/cgroups.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/linux_launcher.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

using state::RunState;

LinuxLauncher::LinuxLauncher(
    const Flags& _flags,
    int _namespaces,
    const string& _hierarchy)
  : flags(_flags),
    namespaces(_namespaces),
    hierarchy(_hierarchy) {}


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy, "freezer", flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create Linux launcher: " + hierarchy.error());
  }

  LOG(INFO) << "Using " << hierarchy.get()
            << " as the freezer hierarchy for the Linux launcher";

  // TODO(idownes): Inspect the isolation flag to determine namespaces to use.
  int namespaces = 0;

  return new LinuxLauncher(flags, namespaces, hierarchy.get());
}


Try<Nothing> LinuxLauncher::recover(const std::list<state::RunState>& states)
{
  hashset<string> cgroups;

  foreach (const RunState& state, states) {
    if (state.id.isNone()) {
      return Error("ContainerID is required to recover");
    }
    const ContainerID& containerId = state.id.get();

    Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));

    if (!exists.get()) {
      // This may occur if the freezer cgroup was destroyed but the slave dies
      // before noticing this.
      // The containerizer will monitor the container's pid and notice that it
      // has exited, triggering destruction of the container.
      LOG(INFO) << "Couldn't find freezer cgroup for container " << containerId;
      continue;
    }

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

    cgroups.insert(cgroup(containerId));
  }

  Try<vector<string> > orphans = cgroups::get(hierarchy, flags.cgroups_root);
  if (orphans.isError()) {
    return Error(orphans.error());
  }

  foreach (const string& orphan, orphans.get()) {
    if (!cgroups.contains(orphan)) {
      LOG(INFO) << "Removing orphaned cgroup"
                << " '" << path::join("freezer", orphan) << "'";
      cgroups::destroy(hierarchy, orphan);
    }
  }

  return Nothing();
}


// Helper for clone() which expects an int(void*).
static int childMain(void* child)
{
  const lambda::function<int()>* func =
    static_cast<const lambda::function<int()>*> (child);

  return (*func)();
}


// Helper that creates a new session then blocks on reading the pipe before
// calling the supplied function.
static int _childMain(
    const lambda::function<int()>& childFunction,
    int pipes[2])
{
  // In child.
  os::close(pipes[1]);

  // Move to a different session (and new process group) so we're independent
  // from the slave's session (otherwise children will receive SIGHUP if the
  // slave exits).
  // TODO(idownes): perror is not listed as async-signal-safe and should be
  // reimplemented safely.
  if (setsid() == -1) {
    perror("Failed to put child in a new session");
    os::close(pipes[0]);
    _exit(1);
  }

  // Do a blocking read on the pipe until the parent signals us to continue.
  int buf;
  int len;
  while ((len = read(pipes[0], &buf, sizeof(buf))) == -1 && errno == EINTR);

  if (len != sizeof(buf)) {
    ABORT("Failed to synchronize with parent");
  }

  os::close(pipes[0]);

  // This function should exec() and therefore not return.
  childFunction();

  ABORT("Child failed to exec");

  return -1;
}


Try<pid_t> LinuxLauncher::fork(
    const ContainerID& containerId,
    const lambda::function<int()>& childFunction)
{
  // Create a freezer cgroup for this container if necessary.
  Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));
  if (exists.isError()) {
    return Error("Failed to check existence of freezer cgroup: " +
                 exists.error());
  }

  if (!exists.get()) {
    Try<Nothing> created = cgroups::create(hierarchy, cgroup(containerId));

    if (created.isError()) {
      return Error("Failed to create freezer cgroup: " + created.error());
    }
  }

  // Use a pipe to block the child until it's been moved into the freezer
  // cgroup.
  int pipes[2];
  // We assume this should not fail under reasonable conditions so we use CHECK.
  CHECK(pipe(pipes) == 0);

  // Use the _childMain helper which moves the child into a new session and
  // blocks on the pipe until we're ready for it to run.
  lambda::function<int()> func =
    lambda::bind(&_childMain, childFunction, pipes);

  // Stack for the child.
  // - unsigned long long used for best alignment.
  // - static is ok because each child gets their own copy after the clone.
  // - 8 MiB appears to be the default for "ulimit -s" on OSX and Linux.
  static unsigned long long stack[(8*1024*1024)/sizeof(unsigned long long)];

  LOG(INFO) << "Cloning child process with flags = " << namespaces;

  pid_t pid;
  if ((pid = ::clone(
          childMain,
          &stack[sizeof(stack)/sizeof(stack[0]) - 1],  // stack grows down
          namespaces | SIGCHLD,   // Specify SIGCHLD as child termination signal
          static_cast<void*>(&func))) == -1) {
      return ErrnoError("Failed to clone child process");
  }

  // Parent.
  os::close(pipes[0]);

  // Move the child into the freezer cgroup. Any grandchildren will also be
  // contained in the cgroup.
  Try<Nothing> assign = cgroups::assign(hierarchy, cgroup(containerId), pid);

  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign process " << pid
                << " of container '" << containerId << "'"
                << " to its freezer cgroup: " << assign.error();
    kill(pid, SIGKILL);
    return Error("Failed to contain process");
  }

  // Now that we've contained the child we can signal it to continue by
  // writing to the pipe.
  int buf;
  ssize_t len;
  while ((len = write(pipes[1], &buf, sizeof(buf))) == -1 && errno == EINTR);

  if (len != sizeof(buf)) {
    // Ensure the child is killed.
    kill(pid, SIGKILL);
    os::close(pipes[1]);
    return Error("Failed to synchronize child process");
  }
  os::close(pipes[1]);

  // Store the pid (session id and process group id) if this is the first
  // process forked for this container.
  if (!pids.contains(containerId)) {
    pids.put(containerId, pid);
  }

  return pid;
}


Future<Nothing> _destroy(
    const ContainerID& containerId,
    process::Future<bool> destroyed)
{
  if (destroyed.isFailed()) {
    LOG(ERROR) << "Failed to destroy freezer cgroup for '"
               << containerId << "': " << destroyed.failure();
    return Failure("Failed to destroy launcher: " + destroyed.failure());
  }
  return Nothing();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  pids.erase(containerId);

  return cgroups::destroy(hierarchy, cgroup(containerId))
    .then(lambda::bind(&_destroy, containerId, lambda::_1));
}


string LinuxLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(flags.cgroups_root, containerId.value());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
