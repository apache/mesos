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

#include <vector>

#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/unreachable.hpp>

#include "linux/cgroups.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/cgroups_launcher.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

using state::RunState;

CgroupsLauncher::CgroupsLauncher(const Flags& _flags, const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


Try<Launcher*> CgroupsLauncher::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy, "freezer", flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create cgroups launcher: " + hierarchy.error());
  }

  LOG(INFO) << "Using " << hierarchy.get()
            << " as the freezer hierarchy for the cgroups launcher";

  return new CgroupsLauncher(flags, hierarchy.get());
}


Try<Nothing> CgroupsLauncher::recover(const std::list<state::RunState>& states)
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


Try<pid_t> CgroupsLauncher::fork(
    const ContainerID& containerId,
    const lambda::function<int()>& inChild)
{
  // Create a freezer cgroup for this container if necessary.
  Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));

  if (exists.isError()) {
    return Error("Failed to create freezer cgroup: " + exists.error());
  }

  if (!exists.get()) {
    Try<Nothing> created = cgroups::create(hierarchy, cgroup(containerId));

    if (created.isError()) {
      LOG(ERROR) << "Failed to create freezer cgroup for container '"
                 << containerId << "': " << created.error();
      return Error("Failed to contain process: " + created.error());
    }
  }

  // Additional processes forked will be put into the same process group and
  // session.
  Option<pid_t> pgid = pids.get(containerId);

  // Use a pipe to block the child until it's been moved into the freezer
  // cgroup.
  int pipes[2];
  // We assume this should not fail under reasonable conditions so we use CHECK.
  CHECK(pipe(pipes) == 0);

  pid_t pid;

  if ((pid = ::fork()) == -1) {
    return ErrnoError("Failed to fork");
  }

  if (pid == 0) {
    // In child.
    os::close(pipes[1]);

    // Move to a previously created process group (and session) if available,
    // else create a new session and process group. Even though we track
    // processes using cgroups we need to move to a different session so we're
    // independent from the slave's session (otherwise children will receive
    // SIGHUP if the slave exits).
    // TODO(idownes): perror is not listed as async-signal-safe and should be
    // reimplemented safely.
    if (pgid.isSome() && (setpgid(0, pgid.get()) == -1)) {
      perror("Failed to put child into process group");
      os::close(pipes[0]);
      _exit(1);
    } else if (setsid() == -1) {
      perror("Failed to put child in a new session");
      os::close(pipes[0]);
      _exit(1);
    }

    // Do a blocking read on the pipe until the parent signals us to continue.
    int buf;
    int len;
    while ((len = read(pipes[0], &buf, sizeof(buf))) == -1 && errno == EINTR);

    if (len != sizeof(buf)) {
      const char* message = "Failed to synchronize with parent";
      // Ignore the return value from write() to silence compiler warning.
      while (write(STDERR_FILENO, message, strlen(message)) == -1 &&
          errno == EINTR);
      os::close(pipes[0]);
      _exit(1);
    }

    os::close(pipes[0]);

    // This function should exec() and therefore not return.
    inChild();

    const char* message = "Child failed to exec";
    while (write(STDERR_FILENO, message, strlen(message)) == -1 &&
        errno == EINTR);

    _exit(1);
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


Future<Nothing> CgroupsLauncher::destroy(const ContainerID& containerId)
{
  pids.erase(containerId);

  return cgroups::destroy(hierarchy, cgroup(containerId))
    .then(lambda::bind(&_destroy, containerId, lambda::_1));
}


string CgroupsLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(flags.cgroups_root, containerId.value());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
