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
#include "linux/systemd.hpp"

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


// `_systemdHierarchy` is only set if running on a systemd environment.
LinuxLauncher::LinuxLauncher(
    const Flags& _flags,
    const string& _freezerHierarchy,
    const Option<string>& _systemdHierarchy)
  : flags(_flags),
    freezerHierarchy(_freezerHierarchy),
    systemdHierarchy(_systemdHierarchy) {}


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<string> freezerHierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "freezer",
      flags.cgroups_root);

  if (freezerHierarchy.isError()) {
    return Error(
        "Failed to create Linux launcher: " + freezerHierarchy.error());
  }

  // Ensure that no other subsystem is attached to the freezer hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(freezerHierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        freezerHierarchy.get());
  } else if (subsystems.get().size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        freezerHierarchy.get());
  }

  LOG(INFO) << "Using " << freezerHierarchy.get()
            << " as the freezer hierarchy for the Linux launcher";

  // On systemd environments we currently migrate executor pids into a separate
  // executor slice. This allows the life-time of the executor to be extended
  // past the life-time of the slave. See MESOS-3352.
  // The LinuxLauncher takes responsibility for creating and starting this
  // slice. It then migrates executor pids into this slice before it "unpauses"
  // the executor. This is the same pattern as the freezer.

  return new LinuxLauncher(
      flags,
      freezerHierarchy.get(),
      systemd::exists() ?
        Some(systemd::hierarchy()) :
        Option<std::string>::none());
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const std::list<ContainerState>& states)
{
  hashset<string> recovered;

  // On systemd environments, capture the pids under the
  // `MESOS_EXECUTORS_SLICE` for validation during recovery.
  Result<std::set<pid_t>> mesosExecutorSlicePids = None();
  if (systemdHierarchy.isSome()) {
    mesosExecutorSlicePids = cgroups::processes(
        systemdHierarchy.get(),
        systemd::mesos::MESOS_EXECUTORS_SLICE);

    // If we error out trying to read the pids from the `MESOS_EXECUTORS_SLICE`
    // we fail. This is a programmer error as we did not set up the slice
    // correctly.
    if (mesosExecutorSlicePids.isError()) {
      return Failure("Failed to read pids from systemd '" +
                     stringify(systemd::mesos::MESOS_EXECUTORS_SLICE) + "'");
    }
  }

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

    Try<bool> exists = cgroups::exists(freezerHierarchy, cgroup(containerId));

    if (!exists.get()) {
      // This may occur if the freezer cgroup was destroyed but the
      // slave dies before noticing this. The containerizer will
      // monitor the container's pid and notice that it has exited,
      // triggering destruction of the container.
      LOG(INFO) << "Couldn't find freezer cgroup for container "
                << containerId << ", assuming already destroyed";
      continue;
    }

    // If we are on a systemd environment, check that the pid is still in the
    // `MESOS_EXECUTORS_SLICE`. If it is not, warn the operator that resource
    // isolation may be invalidated.
    // TODO(jmlvanre): Add a flag that enforces this matching (i.e. exits if a
    // pid was found in the freezer but not in the
    // `MESOS_EXECUTORS_SLICE`. We need to flag to support the upgrade path.
    if (systemdHierarchy.isSome() && mesosExecutorSlicePids.isSome()) {
      if (mesosExecutorSlicePids.get().count(pid) <= 0) {
        LOG(WARNING)
          << "Couldn't find pid '" << pid << "' in '"
          << systemd::mesos::MESOS_EXECUTORS_SLICE << "'. This can lead to"
          << " lack of proper resource isolation";
      }
    }

    recovered.insert(cgroup(containerId));
  }

  // Return the set of orphan containers.
  Try<vector<string>> cgroups =
    cgroups::get(freezerHierarchy, flags.cgroups_root);

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


static int childSetup(
    int pipes[2],
    const Option<lambda::function<int()>>& setup)
{
  // In child.
  ::close(pipes[1]);

  // Do a blocking read on the pipe until the parent signals us to
  // continue.
  char dummy;
  ssize_t length;
  while ((length = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    ABORT("Failed to synchronize with parent");
  }

  ::close(pipes[0]);

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
  Try<bool> exists = cgroups::exists(freezerHierarchy, cgroup(containerId));
  if (exists.isError()) {
    return Error("Failed to check existence of freezer cgroup: " +
                 exists.error());
  }

  if (!exists.get()) {
    Try<Nothing> created =
      cgroups::create(freezerHierarchy, cgroup(containerId));

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

  int cloneFlags = namespaces.isSome() ? namespaces.get() : 0;
  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  LOG(INFO) << "Cloning child process with flags = "
            << ns::stringify(cloneFlags);

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
      flags,
      environment,
      lambda::bind(&childSetup, pipes, setup),
      lambda::bind(&os::clone, lambda::_1, cloneFlags),
      // TODO(jmlvanre): Use systemd hook.
      Subprocess::Hook::None());

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
      freezerHierarchy,
      cgroup(containerId),
      child.get().pid());

  if (assign.isError()) {
    LOG(ERROR) << "Failed to assign process " << child.get().pid()
                << " of container '" << containerId << "'"
                << " to its freezer cgroup: " << assign.error();

    ::kill(child.get().pid(), SIGKILL);
    return Error("Failed to contain process");
  }

  // If we are on systemd, then move the child into the
  // `MESOS_EXECUTORS_SLICE`. As with the freezer, any grandchildren will also
  // be contained in the slice.
  if (systemdHierarchy.isSome()) {
    Try<Nothing> assign = cgroups::assign(
        systemdHierarchy.get(),
        systemd::mesos::MESOS_EXECUTORS_SLICE,
        child.get().pid());

    if (assign.isError()) {
      LOG(ERROR) << "Failed to assign process " << child.get().pid()
                  << " of container '" << containerId << "'"
                  << " to its systemd executor slice: " << assign.error();

      ::kill(child.get().pid(), SIGKILL);
      return Error("Failed to contain process on systemd");
    }

    LOG(INFO) << "Assigned child process '" << child.get().pid() << "' to '"
              << systemd::mesos::MESOS_EXECUTORS_SLICE << "'";
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
  Try<bool> exists = cgroups::exists(freezerHierarchy, cgroup(containerId));
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
            freezerHierarchy,
            cgroup(containerId),
            cgroups::DESTROY_TIMEOUT));
  }

  // Try to clean up using just the freezer cgroup.
  return cgroups::destroy(
      freezerHierarchy,
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
