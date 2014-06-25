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

#include <process/collect.hpp>

#include <stout/abort.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/linux_launcher.hpp"

using namespace process;

using std::list;
using std::map;
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


// An old glibc might not have this symbol.
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "freezer",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create Linux launcher: " + hierarchy.error());
  }

  LOG(INFO) << "Using " << hierarchy.get()
            << " as the freezer hierarchy for the Linux launcher";

  // TODO(idownes): Inspect the isolation flag to determine namespaces
  // to use.
  int namespaces = 0;

#ifdef WITH_NETWORK_ISOLATOR
  // The network port mapping isolator requires network (CLONE_NEWNET)
  // and mount (CLONE_NEWNS) namespaces.
  if (strings::contains(flags.isolation, "network/port_mapping")) {
    namespaces |= CLONE_NEWNET;
    namespaces |= CLONE_NEWNS;
  }
#endif

  return new LinuxLauncher(flags, namespaces, hierarchy.get());
}


Future<Nothing> _recover(const Future<list<Nothing> >& futures)
{
  return Nothing();
}


Future<Nothing> LinuxLauncher::recover(const std::list<state::RunState>& states)
{
  hashset<string> cgroups;

  foreach (const RunState& state, states) {
    if (state.id.isNone()) {
      return Failure("ContainerID is required to recover");
    }
    const ContainerID& containerId = state.id.get();

    Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));

    if (!exists.get()) {
      // This may occur if the freezer cgroup was destroyed but the
      // slave dies before noticing this. The containerizer will
      // monitor the container's pid and notice that it has exited,
      // triggering destruction of the container.
      LOG(INFO) << "Couldn't find freezer cgroup for container " << containerId;
      continue;
    }

    if (state.forkedPid.isNone()) {
      return Failure("Executor pid is required to recover container " +
                     stringify(containerId));
    }
    pid_t pid = state.forkedPid.get();

    if (pids.containsValue(pid)) {
      // This should (almost) never occur. There is the possibility
      // that a new executor is launched with the same pid as one that
      // just exited (highly unlikely) and the slave dies after the
      // new executor is launched but before it hears about the
      // termination of the earlier executor (also unlikely).
      // Regardless, the launcher can't do anything sensible so this
      // is considered an error.
      return Failure("Detected duplicate pid " + stringify(pid) +
                     " for container " + stringify(containerId));
    }

    pids.put(containerId, pid);

    cgroups.insert(cgroup(containerId));
  }

  Try<vector<string> > orphans = cgroups::get(hierarchy, flags.cgroups_root);
  if (orphans.isError()) {
    return Failure(orphans.error());
  }

  list<Future<Nothing> > futures;

  foreach (const string& orphan, orphans.get()) {
    if (!cgroups.contains(orphan)) {
      LOG(INFO) << "Removing orphaned cgroup"
                << " '" << path::join("freezer", orphan) << "'";
      // We must wait for all cgroups to be destroyed; isolators
      // assume all processes have been terminated for any orphaned
      // containers before Isolator::recover() is called.
      futures.push_back(
          cgroups::destroy(hierarchy, orphan, cgroups::DESTROY_TIMEOUT));
    }
  }

  return collect(futures)
    .then(lambda::bind(&_recover, lambda::_1));
}


// Helper for clone() which expects an int(void*).
static int childMain(void* _func)
{
  const lambda::function<int()>* func =
    static_cast<const lambda::function<int()>*> (_func);

  return (*func)();
}


// The customized clone function which will be used by 'subprocess()'.
static pid_t clone(const lambda::function<int()>& func, int namespaces)
{
  // Stack for the child.
  // - unsigned long long used for best alignment.
  // - static is ok because each child gets their own copy after the clone.
  // - 8 MiB appears to be the default for "ulimit -s" on OSX and Linux.
  static unsigned long long stack[(8*1024*1024)/sizeof(unsigned long long)];

  LOG(INFO) << "Cloning child process with flags = " << namespaces;

  return ::clone(
      childMain,
      &stack[sizeof(stack)/sizeof(stack[0]) - 1],  // stack grows down
      namespaces | SIGCHLD,   // Specify SIGCHLD as child termination signal
      (void*) &func);
}


static int childSetup(
    int pipes[2],
    const Option<lambda::function<int()> >& setup)
{
  // In child.
  while (::close(pipes[1]) == -1 && errno == EINTR);

  // Do a blocking read on the pipe until the parent signals us to
  // continue.
  char dummy;
  ssize_t length;
  while ((length = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    ABORT("Failed to synchronize with parent");
  }

  while (::close(pipes[0]) == -1 && errno == EINTR);

  // Move to a different session (and new process group) so we're
  // independent from the slave's session (otherwise children will
  // receive SIGHUP if the slave exits).
  // TODO(idownes): perror is not listed as async-signal-safe and
  // should be reimplemented safely.
  // TODO(jieyu): Move this logic to the subprocess (i.e.,
  // mesos-containerizer launch).
  if (::setsid() == -1) {
    perror("Failed to put child in a new session");
    return 1;
  }

  if (setup.isSome()) {
    return setup.get()();
  }

  return 0;
}


Try<pid_t> LinuxLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const process::Subprocess::IO& in,
    const process::Subprocess::IO& out,
    const process::Subprocess::IO& err,
    const Option<flags::FlagsBase>& flags,
    const Option<map<string, string> >& environment,
    const Option<lambda::function<int()> >& setup)
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

  // Use a pipe to block the child until it's been moved into the
  // freezer cgroup.
  int pipes[2];

  // We assume this should not fail under reasonable conditions so we
  // use CHECK.
  CHECK_EQ(0, ::pipe(pipes));

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
      flags,
      environment,
      lambda::bind(&childSetup, pipes, setup),
      lambda::bind(&clone, lambda::_1, namespaces));

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
  }

  // Parent.
  os::close(pipes[0]);

  // Move the child into the freezer cgroup. Any grandchildren will
  // also be contained in the cgroup.
  // TODO(jieyu): Move this logic to the subprocess (i.e.,
  // mesos-containerizer launch).
  Try<Nothing> assign = cgroups::assign(
      hierarchy,
      cgroup(containerId),
      child.get().pid());

  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign process " << child.get().pid()
                << " of container '" << containerId << "'"
                << " to its freezer cgroup: " << assign.error();

    ::kill(child.get().pid(), SIGKILL);
    return Error("Failed to contain process");
  }

  // Now that we've contained the child we can signal it to continue
  // by writing to the pipe.
  char dummy;
  ssize_t length;
  while ((length = ::write(pipes[1], &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  os::close(pipes[1]);

  if (length != sizeof(dummy)) {
    // Ensure the child is killed.
    ::kill(child.get().pid(), SIGKILL);
    return Error("Failed to synchronize child process");
  }

  // Store the pid (session id and process group id) if this is the
  // first process forked for this container.
  if (!pids.contains(containerId)) {
    pids.put(containerId, child.get().pid());
  }

  return child.get().pid();
}


Future<Nothing> _destroy(
    const ContainerID& containerId,
    const process::Future<Nothing>& destroyed)
{
  if (!destroyed.isReady()) {
    return Failure("Failed to destroy launcher: " +
                   (destroyed.isFailed() ? destroyed.failure() : "discarded"));
  }

  return Nothing();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  pids.erase(containerId);

  return cgroups::destroy(
      hierarchy, cgroup(containerId), cgroups::DESTROY_TIMEOUT)
    .onAny(lambda::bind(&_destroy, containerId, lambda::_1));
}


string LinuxLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(flags.cgroups_root, containerId.value());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
