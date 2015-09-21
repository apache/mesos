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
#include <stout/check.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/isolators/namespaces/pid.hpp"

using namespace process;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

static ContainerID container(const string& cgroup)
{
  string basename = Path(cgroup).basename();

  ContainerID containerId;
  containerId.set_value(basename);
  return containerId;
}


LinuxLauncher::LinuxLauncher(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "freezer",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create Linux launcher: " + hierarchy.error());
  }

  // Ensure that no other subsystem is attached to the hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(hierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        hierarchy.get());
  } else if (subsystems.get().size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        hierarchy.get());
  }

  LOG(INFO) << "Using " << hierarchy.get()
            << " as the freezer hierarchy for the Linux launcher";

  return new LinuxLauncher(
      flags,
      hierarchy.get());
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const std::list<ContainerState>& states)
{
  hashset<string> recovered;

  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    pid_t pid = state.pid();

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

    // Store the pid now because if the freezer cgroup is absent
    // (slave terminated after the cgroup is destroyed but before it
    // was notified) then we'll still need it for the check in
    // destroy() when we clean up.
    pids.put(containerId, pid);

    Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));

    if (!exists.get()) {
      // This may occur if the freezer cgroup was destroyed but the
      // slave dies before noticing this. The containerizer will
      // monitor the container's pid and notice that it has exited,
      // triggering destruction of the container.
      LOG(INFO) << "Couldn't find freezer cgroup for container "
                << containerId << ", assuming already destroyed";
      continue;
    }

    recovered.insert(cgroup(containerId));
  }

  // Return the set of orphan containers.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    if (!recovered.contains(cgroup)) {
      orphans.insert(container(cgroup));
    }
  }

  return orphans;
}


// Helper for clone() which expects an int(void*).
static int childMain(void* _func)
{
  const lambda::function<int()>* func =
    static_cast<const lambda::function<int()>*> (_func);

  return (*func)();
}


// The customized clone function which will be used by 'subprocess()'.
static pid_t clone(
    const lambda::function<int()>& func,
    const Option<int>& namespaces)
{
  // Stack for the child.
  // - unsigned long long used for best alignment.
  // - static is ok because each child gets their own copy after the clone.
  // - 8 MiB appears to be the default for "ulimit -s" on OSX and Linux.
  //
  // NOTE: We need to allocate the stack dynamically. This is because
  // glibc's 'clone' will modify the stack passed to it, therefore the
  // stack must NOT be shared as multiple 'clone's can be invoked
  // simultaneously.
  int stackSize = 8 * 1024 * 1024;

  unsigned long long *stack =
    new unsigned long long[stackSize/sizeof(unsigned long long)];

  int flags = namespaces.isSome() ? namespaces.get() : 0;
  flags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  LOG(INFO) << "Cloning child process with flags = "
            << ns::stringify(flags);

  pid_t pid = ::clone(
      childMain,
      &stack[stackSize/sizeof(stack[0]) - 1],  // stack grows down.
      flags,
      (void*) &func);

  delete[] stack;

  return pid;
}


static int childSetup(
    int pipes[2],
    const Option<lambda::function<int()>>& setup)
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
    const Option<map<string, string>>& environment,
    const Option<lambda::function<int()>>& setup,
    const Option<int>& namespaces)
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

  if (!pids.contains(containerId)) {
    pids.put(containerId, child.get().pid());
  }

  return child.get().pid();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  if (!pids.contains(containerId) && !orphans.contains(containerId)) {
    return Failure("Unknown container");
  }

  pids.erase(containerId);
  orphans.erase(containerId);

  // Just return if the cgroup was destroyed and the slave didn't receive the
  // notification. See comment in recover().
  Try<bool> exists = cgroups::exists(hierarchy, cgroup(containerId));
  if (exists.isError()) {
    return Failure("Failed to check existence of freezer cgroup: " +
                   exists.error());
  }

  if (!exists.get()) {
    return Nothing();
  }

  Result<ino_t> containerPidNs =
    NamespacesPidIsolatorProcess::getNamespace(containerId);

  if (containerPidNs.isSome()) {
    LOG(INFO) << "Using pid namespace to destroy container " << containerId;

    return ns::pid::destroy(containerPidNs.get())
      .then(lambda::bind(
            (Future<Nothing>(*)(const string&,
                                const string&,
                                const Duration&))(&cgroups::destroy),
            hierarchy,
            cgroup(containerId),
            cgroups::DESTROY_TIMEOUT));
  }

  // Try to clean up using just the freezer cgroup.
  return cgroups::destroy(
      hierarchy,
      cgroup(containerId),
      cgroups::DESTROY_TIMEOUT);
}


string LinuxLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(flags.cgroups_root, containerId.value());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
