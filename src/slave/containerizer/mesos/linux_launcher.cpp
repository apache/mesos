// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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

#include "slave/containerizer/mesos/linux_launcher.hpp"

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

constexpr char LINUX_LAUNCHER_NAME[] = "linux";


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
      systemd::enabled() ?
        Some(systemd::hierarchy()) :
        Option<string>::none());
}


bool LinuxLauncher::available()
{
  // Make sure:
  //   - we run as root
  //   - "freezer" subsytem is enabled.

  Try<bool> freezer = cgroups::enabled("freezer");
  return ::geteuid() == 0 &&
         freezer.isSome() &&
         freezer.get();
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const list<ContainerState>& states)
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


// A hook that is executed in the parent process. It attempts to move a process
// into the freezer cgroup.
//
// NOTE: The child process is blocked by the hook infrastructure while
// these hooks are executed.
// NOTE: Returning an Error implies the child process will be killed.
Try<Nothing> assignFreezerHierarchy(
    pid_t child,
    const string& hierarchy,
    const string& cgroup)
{
  // Create a freezer cgroup for this container if necessary.
  Try<bool> exists = cgroups::exists(hierarchy, cgroup);
  if (exists.isError()) {
    return Error("Failed to assign process to its freezer cgroup: "
                 "Failed to check existence of freezer cgroup: " +
                 exists.error());
  }

  if (!exists.get()) {
    Try<Nothing> created = cgroups::create(hierarchy, cgroup);

    if (created.isError()) {
      return Error("Failed to assign process to its freezer cgroup: "
                   "Failed to create freezer cgroup: " + created.error());
    }
  }

  // Move the child into the freezer cgroup. Any grandchildren will
  // also be contained in the cgroup.
  Try<Nothing> assign = cgroups::assign(hierarchy, cgroup, child);

  if (assign.isError()) {
    return Error("Failed to assign process to its freezer cgroup: " +
                 assign.error());
  }

  return Nothing();
}


Try<pid_t> LinuxLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const process::Subprocess::IO& in,
    const process::Subprocess::IO& out,
    const process::Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& namespaces,
    vector<Subprocess::Hook> parentHooks)
{
  int cloneFlags = namespaces.isSome() ? namespaces.get() : 0;
  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  LOG(INFO) << "Cloning child process with flags = "
            << ns::stringify(cloneFlags);

  // NOTE: Currently we don't care about the order of the hooks, as
  // both hooks are independent.

  // If we are on systemd, then extend the life of the child. As with the
  // freezer, any grandchildren will also be contained in the slice.
  if (systemdHierarchy.isSome()) {
    parentHooks.emplace_back(Subprocess::Hook(&systemd::mesos::extendLifetime));
  }

  // Create parent Hook for moving child into freezer cgroup.
  parentHooks.emplace_back(Subprocess::Hook(lambda::bind(
      &assignFreezerHierarchy,
      lambda::_1,
      freezerHierarchy,
      cgroup(containerId))));

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
      SETSID,
      flags,
      environment,
      lambda::bind(&os::clone, lambda::_1, cloneFlags),
      parentHooks);

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
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

  // Try to clean up using just the freezer cgroup.
  return cgroups::destroy(
      freezerHierarchy,
      cgroup(containerId),
      cgroups::DESTROY_TIMEOUT);
}


Future<ContainerStatus> LinuxLauncher::status(const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    return Failure("Container does not exist!");
  }

  ContainerStatus status;
  status.set_executor_pid(pids[containerId]);

  return status;
}


string LinuxLauncher::getExitStatusCheckpointPath(
    const ContainerID& containerId)
{
  return path::join(
      flags.runtime_dir,
      "launcher",
      LINUX_LAUNCHER_NAME,
      buildPathFromHierarchy(containerId, "containers"),
      "exit_status");
}


string LinuxLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(flags.cgroups_root, containerId.value());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
