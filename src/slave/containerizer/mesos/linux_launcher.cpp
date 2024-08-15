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
#include <stout/stringify.hpp>

#include "linux/cgroups.hpp"
#include "linux/cgroups2.hpp"
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using namespace process;

using lambda::function;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

// Launcher for Linux systems with cgroups. Uses a freezer cgroup to
// track pids.
class LinuxLauncherProcess : public Process<LinuxLauncherProcess>
{
public:
  LinuxLauncherProcess(
      const Flags& flags,
      bool cgroupsV2,
      const Option<string>& freezerHierarchy,
      const Option<string>& systemdHierarchy);

  virtual Future<hashset<ContainerID>> recover(
      const vector<mesos::slave::ContainerState>& states);

  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const string& path,
      const vector<string>& argv,
      const mesos::slave::ContainerIO& containerIO,
      const flags::FlagsBase* flags,
      const Option<map<string, string>>& environment,
      const Option<int>& enterNamespaces,
      const Option<int>& cloneNamespaces,
      const vector<int_fd>& whitelistFds);

  virtual Future<Nothing> destroy(const ContainerID& containerId);

  virtual Future<ContainerStatus> status(
      const ContainerID& containerId);

private:
  // Helper struct for storing information about each container. A
  // "container" here means a cgroup in the freezer subsystem that is
  // used to represent a collection of processes. This container may
  // also have multiple namespaces associated with it but that is not
  // managed explicitly here.
  struct Container
  {
    ContainerID id;

    // NOTE: this represents the "init" of the container that we
    // created (it may be for an executor, or any arbitrary process
    // that has been launched in the event of nested containers).
    //
    // This is none when it's an orphan container (i.e., we have a
    // freezer cgroup but we were not expecting the container during
    // `LinuxLauncher::recover`).
    Option<pid_t> pid = None();
  };

  Try<Nothing> recoverContainersFromCgroups();
  Try<Nothing> recoverContainersFromCgroups2();

  Future<Nothing> destroyCgroups(const Container& container);
  Future<Nothing> _destroyCgroups(const Container& container);
  Future<Nothing> destroyCgroups2(const Container& container);

  const Flags flags;

  struct CgroupsInfo
  {
    // Flag indicating whether cgroups v2 is used.
    bool v2;

    // Absolute path of the cgroup freezer hierarchy.
    // Only present when cgroups v1 is used.
    Option<string> freezerHierarchy;

    // Absolute path the systemd hierarchy.
    // Only present in cgroups v1 when systemd is enabled
    // (i.e. `systemd::enabled()`).
    Option<string> systemdHierarchy;
  } cgroupsInfo;

  hashmap<ContainerID, Container> containers;
};


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<bool> mounted = cgroups2::mounted();
  if (mounted.isError()) {
    return Error("Failed to check if cgroups2 is mounted: " + mounted.error());
  }
  if (*mounted) {
    return createCgroups2Launcher(flags);
  }
  return createCgroupsLauncher(flags);
}


Try<Launcher*> LinuxLauncher::createCgroupsLauncher(const Flags& flags)
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
  Try<set<string>> subsystems = cgroups::subsystems(*freezerHierarchy);
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        *freezerHierarchy);
  } else if (subsystems->size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        *freezerHierarchy);
  }

  LOG(INFO) << "Using " << *freezerHierarchy
            << " as the freezer hierarchy for the Linux launcher";

  // On systemd environments, we currently do the following:
  //
  // (1) For Mesos version < 1.6, we migrate executor pids into a
  // separate slice. This allows the life-time of the executor to be
  // extended past the life-time of the slave. See MESOS-3352.
  //
  // (2) For Mesos version >= 1.6, we create the cgroups under the
  // systemd hierarchy directly, and use the same layout as that in
  // the freezer hierarchy. This should prevent systemd from migrating
  // pids in other hierarchies as well because the pids are unknown to
  // systemd.
  Option<string> systemdHierarchy;

  if (systemd::enabled()) {
    systemdHierarchy = systemd::hierarchy();

    // Create the root cgroup if does not exist.
    if (!cgroups::exists(*systemdHierarchy, flags.cgroups_root)) {
      Try<Nothing> create = cgroups::create(
          *systemdHierarchy,
          flags.cgroups_root);

      if (create.isError()) {
        return Error(
            "Failed to create cgroup root under systemd hierarchy: " +
            create.error());
      }
    }

    LOG(INFO) << "Using " << *systemdHierarchy
              << " as the systemd hierarchy for the Linux launcher";
  }

  return new LinuxLauncher(flags, false, *freezerHierarchy, systemdHierarchy);
}


Try<Launcher*> LinuxLauncher::createCgroups2Launcher(const Flags& flags)
{
  return new LinuxLauncher(flags, true, None(), None());
}


bool LinuxLauncher::available()
{
  bool available = false;

  // Check if cgroups v2 is available.
  Try<bool> mounted = cgroups2::mounted();
  available |= mounted.isSome() && *mounted;

  // Check if cgroups v1 is available.
  // Make sure:
  // 1. Are running as root.
  // 2. 'freezer' subsystem is enabled.
  Try<bool> freezer = cgroups::enabled("freezer");
  available |= ::geteuid() == 0 && freezer.isSome() && *freezer;

  return available;
}


LinuxLauncher::LinuxLauncher(
    const Flags& flags,
    bool cgroupsV2,
    const Option<string>& freezerHierarchy,
    const Option<string>& systemdHierarchy)
  : process(new LinuxLauncherProcess(
      flags, cgroupsV2, freezerHierarchy, systemdHierarchy))
{
  process::spawn(process.get());
}


LinuxLauncher::~LinuxLauncher()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const vector<mesos::slave::ContainerState>& states)
{
  return dispatch(process.get(), &LinuxLauncherProcess::recover, states);
}


Try<pid_t> LinuxLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const mesos::slave::ContainerIO& containerIO,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces,
    const vector<int_fd>& whitelistFds)
{
  return dispatch(
      process.get(),
      &LinuxLauncherProcess::fork,
      containerId,
      path,
      argv,
      containerIO,
      flags,
      environment,
      enterNamespaces,
      cloneNamespaces,
      whitelistFds).get();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  return dispatch(process.get(), &LinuxLauncherProcess::destroy, containerId);
}


Future<ContainerStatus> LinuxLauncher::status(
    const ContainerID& containerId)
{
  return dispatch(process.get(), &LinuxLauncherProcess::status, containerId);
}


LinuxLauncherProcess::LinuxLauncherProcess(
    const Flags& flags,
    bool cgroupsV2,
    const Option<string>& freezerHierarchy,
    const Option<string>& systemdHierarchy)
  : flags(flags)
{
  cgroupsInfo.v2 = cgroupsV2;
  cgroupsInfo.freezerHierarchy = freezerHierarchy;
  cgroupsInfo.systemdHierarchy = systemdHierarchy;
}


Future<hashset<ContainerID>> LinuxLauncherProcess::recover(
    const vector<ContainerState>& states)
{
  // 1. Cgroups v1: We recover container ids by looking at the cgroups in the
  //                freezer or (optional) systemd hierarchy.
  //    Cgroups v2: We recover container ids by looking at the cgroups in the
  //                `flags.cgroups_root` directory, in `/sys/fs/cgroup`.
  //
  // 2. Create a list of the container ids that we expect to recover
  //    based on the persisted `ContainerState`s. If a container is expected
  //    to be recovered but was not found while parsing the cgroups, we create
  //    a container for it from the `ContainerState`, so
  //    `LinuxLauncher::destroy` doesn't fail when called for this container.
  //
  // 3. Cgroups v1: In a systemd environment, check that the container pids are
  //                being managed by either the Mesos `systemd` cgroup or the
  //                systemd `MESOS_EXECUTORS_SLICE`.
  //
  // 4. Return a list of "orphan" containers, that is, containers that were
  //    recovered but were not expected to be recovered.
  LOG(INFO) << "Recovering Linux launcher";

  // Recover containers by looking at the cgroups found in the cgroups
  // filesystem.
  Try<Nothing> recover = cgroupsInfo.v2
    ? recoverContainersFromCgroups2()
    : recoverContainersFromCgroups();

  if (recover.isError()) {
    return Failure("Failed to recover containers from the cgroup filesystem: "
                   + recover.error());
  }

  // Now loop through the containers expected by ContainerState so we
  // can have a complete list of the containers we might ever want to
  // destroy as well as be able to determine orphans below.
  hashset<ContainerID> expected = {};

  foreach (const ContainerState& state, states) {
    expected.insert(state.container_id());

    if (!containers.contains(state.container_id())) {
      // Given that the container was not recovered, that implies that
      // the container was previously destroyed.
      //
      // We still add it to `containers` so that when `LinuxLauncher::destroy`
      // gets called for this container below we don't fail.
      Container container;
      container.id = state.container_id();
      container.pid = state.pid();

      containers.put(container.id, container);

      LOG(INFO) << "Recovered (destroyed) container " << container.id;
    } else {
      // This container exists, so we save the pid so we can check
      // that it's part of the systemd "Mesos executor slice" below.
      containers[state.container_id()].pid = state.pid();
    }
  }

  if (!cgroupsInfo.v2) {
    // TODO(benh): In the past we used to make sure that we didn't have
    // multiple containers that had the same pid. This seemed pretty
    // random, and is highly unlikely to occur in practice. That being
    // said, a good sanity check we could do here is to make sure that
    // the pid is actually contained within each container's freezer
    // cgroup.

    // If we are on a systemd environment, check that container pids are
    // either in the `MESOS_EXECUTORS_SLICE`, or under Mesos cgroup root
    // under the systemd hierarchy. If they are not, warn the operator
    // that resource isolation may be invalidated.
    //
    // TODO(jmlvanre): Add a flag that enforces this rather than just
    // logs a warning (i.e., we exit if a pid was found in the freezer
    // but not in the `MESOS_EXECUTORS_SLICE` or Mesos cgroup root under
    // the systemd hierarhcy). We need a flag to support the upgrade
    // path.
    if (cgroupsInfo.systemdHierarchy.isSome()) {
      foreachvalue (const Container& container, containers) {
        if (container.pid.isNone()) {
          continue;
        }

        pid_t pid = container.pid.get();

        // No need to proceed the check if the pid does not exist
        // anymore. This is possible if the container terminates when
        // the agent is down.
        if (os::kill(pid, 0) != 0) {
          continue;
        }

        Result<string> cgroup = cgroups::named::cgroup("systemd", pid);
        if (!cgroup.isSome()) {
          LOG(ERROR) << "Failed to get cgroup in systemd hierarchy for "
                    << "container pid " << pid << ": "
                    << (cgroup.isError() ? cgroup.error() : "Not found");
          continue;
        }

        bool inMesosCgroupRoot =
          strings::startsWith(*cgroup, flags.cgroups_root);

        bool inMesosExecutorSlice =
          strings::contains(*cgroup, systemd::mesos::MESOS_EXECUTORS_SLICE);

        if (!inMesosCgroupRoot && !inMesosExecutorSlice) {
          LOG(WARNING)
            << "Couldn't find pid " << pid << " in either Mesos cgroup root '"
            << flags.cgroups_root << "' under systemd hierarchy, or systemd "
            << "slice '" << systemd::mesos::MESOS_EXECUTORS_SLICE << "'; "
            << "This can lead to lack of proper resource isolation";
        }
      }
    }
  }

  // Return the list of top-level AND nested orphaned containers,
  // i.e., a container that we recovered but was not expected during
  // recovery.
  hashset<ContainerID> orphans = {};

  foreachvalue (const Container& container, containers) {
    if (!expected.contains(container.id)) {
      LOG(INFO) << container.id << " is a known orphaned container";
      orphans.insert(container.id);
    }
  }

  return orphans;
}


Try<Nothing> LinuxLauncherProcess::recoverContainersFromCgroups()
{
  // Recover all of the "containers" we know about based on the
  // existing cgroups. Note that we check both the freezer hierarchy
  // and the systemd hierarchy (if enabled), and combine the results.
  hashset<string> cgroups;

  Try<vector<string>> freezerCgroups =
    cgroups::get(*cgroupsInfo.freezerHierarchy, flags.cgroups_root);

  if (freezerCgroups.isError()) {
    return Error(
        "Failed to get cgroups from "
        + path::join(*cgroupsInfo.freezerHierarchy, flags.cgroups_root)
        + ": " + freezerCgroups.error());
  }

  foreach (const string& cgroup, freezerCgroups.get()) {
    cgroups.insert(cgroup);
  }

  if (cgroupsInfo.systemdHierarchy.isSome()) {
    Try<vector<string>> systemdCgroups =
      cgroups::get(*cgroupsInfo.systemdHierarchy, flags.cgroups_root);

    if (systemdCgroups.isError()) {
      return Error(
          "Failed to get cgroups from " +
          path::join(*cgroupsInfo.systemdHierarchy, flags.cgroups_root) +
          ": " + systemdCgroups.error());
    }

    foreach (const string& cgroup, systemdCgroups.get()) {
      cgroups.insert(cgroup);
    }
  }

  foreach (const string& cgroup, cgroups) {
    // Need to parse the cgroup to see if it's one we created (i.e.,
    // matches our separator structure) or one that someone else
    // created (e.g., in the future we might have nested containers
    // that are managed by something else rooted within the freezer
    // hierarchy).
    Option<ContainerID> containerId =
      containerizer::paths::parseCgroupPath(flags.cgroups_root, cgroup);

    if (containerId.isNone()) {
      LOG(INFO) << "Not recovering cgroup " << cgroup;
      continue;
    }

    Container container;
    container.id = containerId.get();

    // Add this to `containers` so when `destroy` gets called we
    // properly destroy the container, even if we determine it's an
    // orphan below.
    containers.put(container.id, container);

    LOG(INFO) << "Recovered container " << container.id;
  }

  return Nothing();
}


Try<Nothing> LinuxLauncherProcess::recoverContainersFromCgroups2()
{
  Try<set<string>> cgroups = cgroups2::get(flags.cgroups_root);
  if (cgroups.isError()) {
    return Error("Failed to get cgroups: " + cgroups.error());
  }

  foreach (const string& cgroup, *cgroups) {
    // Parse the cgroups to see if we created them. Add the container ids
    // of the cgroups that parse to `containers` so that on `destroy` they
    // get properly disposed.
    Option<ContainerID> containerId =
      containerizer::paths::cgroups2::containerId(flags.cgroups_root, cgroup);
    if (containerId.isNone()) {
      continue;
    }

    Container container;
    container.id = *containerId;

    containers.put(container.id, container);

    LOG(INFO) << "Recovered container " << container.id;
  }
  return Nothing();
}


Try<pid_t> LinuxLauncherProcess::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const mesos::slave::ContainerIO& containerIO,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces,
    const vector<int_fd>& whitelistFds)
{
  // Make sure this container (nested or not) is unique.
  if (containers.contains(containerId)) {
    return Error("Container '" + stringify(containerId) + "' already exists");
  }

  Option<pid_t> parentPid = None();

  // Ensure nested containers have known parents.
  if (containerId.has_parent()) {
    Option<Container> parent = containers.get(containerId.parent());
    if (parent.isNone()) {
      return Error("Unknown parent container");
    }

    if (parent->pid.isNone()) {
      // TODO(benh): Could also look up a pid in the container and use
      // that in order to enter the namespaces? This would be best
      // effort because we don't know the namespaces that had been
      // created for the original pid.
      return Error("Unknown parent container pid, can not enter namespaces");
    }

    parentPid = parent->pid;
  }

  // Ensure we didn't pass `enterNamespaces`
  // if we aren't forking a nested container.
  if (!containerId.has_parent() && enterNamespaces.isSome()) {
    return Error("Cannot enter parent namespaces for non-nested container");
  }

  int enterFlags = enterNamespaces.getOrElse(0);
  int cloneFlags = cloneNamespaces.getOrElse(0);

  LOG(INFO) << "Launching " << (parentPid.isSome() ? "nested " : "")
            << "container " << containerId << " and cloning with namespaces "
            << ns::stringify(cloneFlags);

  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  vector<Subprocess::ParentHook> parentHooks;
  if (!cgroupsInfo.v2) {
    // The ordering of the hooks is:
    // (1) Create the freezer cgroup, and add the child to the cgroup.
    // (2) Create the systemd cgroup, and add the child to the cgroup.
    //
    // NOTE: The order is important here because the destroy code will
    // always kill the container based on the pids in the freezer
    // cgroup. The systemd cgroup will be removed after that. Therefore,
    // we want to make sure that if the pid is in the systemd cgroup, it
    // must be in the freezer cgroup.

    // Hook for creating and assigning the child into a freezer cgroup.
    parentHooks.emplace_back(Subprocess::ParentHook([=](pid_t child) {
      return cgroups::isolate(
        *cgroupsInfo.freezerHierarchy,
        containerizer::paths::getCgroupPath(
          this->flags.cgroups_root, containerId),
        child);
    }));

    // Hook for creating and assigning the child into a systemd cgroup.
    if (cgroupsInfo.systemdHierarchy.isSome()) {
      parentHooks.emplace_back(Subprocess::ParentHook([=](pid_t child) {
        return cgroups::isolate(
          *cgroupsInfo.systemdHierarchy,
          containerizer::paths::getCgroupPath(
            this->flags.cgroups_root, containerId),
          child);
      }));
    }
  } else {
    parentHooks.emplace_back(
      Subprocess::ParentHook([=](pid_t child) -> Try<Nothing> {
        string leaf = containerizer::paths::cgroups2::container(
          this->flags.cgroups_root, containerId, true);
        CHECK(cgroups2::exists(leaf));

        Try<Nothing> assign = cgroups2::assign(leaf, child);
        if (assign.isError()) {
          return Error("Failed to assign process " + stringify(child)
                       + " to cgroup " + leaf + ": " + assign.error());
        }
        return Nothing();
      }));
  }

  vector<Subprocess::ChildHook> childHooks;

  // Create a new session id so termination signals from the parent process
  // to not terminate the child.
  childHooks.push_back(Subprocess::ChildHook::SETSID());

  Try<Subprocess> child = subprocess(
    path,
    argv,
    containerIO.in,
    containerIO.out,
    containerIO.err,
    flags,
    environment,
    [parentPid, enterFlags, cloneFlags](const lambda::function<int()>& child) {
      if (parentPid.isSome()) {
        Try<pid_t> pid = ns::clone(*parentPid, enterFlags, child, cloneFlags);
        if (pid.isError()) {
          LOG(WARNING) << "Failed to enter namespaces and clone: "
                       << pid.error();
          return -1;
        }
        return *pid;
      } else {
        return os::clone(child, cloneFlags);
      }
    },
    parentHooks,
    childHooks,
    whitelistFds);

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
  }

  Container container;
  container.id = containerId;
  container.pid = child->pid();

  containers.put(container.id, container);

  return container.pid.get();
}


Future<Nothing> LinuxLauncherProcess::destroy(const ContainerID& containerId)
{
  LOG(INFO) << "Asked to destroy container " << containerId;

  Option<Container> container = containers.get(containerId);

  if (container.isNone()) {
    return Nothing();
  }

  // Check if `container` has any nested containers.
  foreachkey (const ContainerID& id, containers) {
    if (id.has_parent()) {
      if (container->id == id.parent()) {
        return Failure("Container has nested containers");
      }
    }
  }

  return cgroupsInfo.v2
    ? destroyCgroups2(*container)
    : destroyCgroups(*container);
}


Future<Nothing> LinuxLauncherProcess::destroyCgroups(const Container& container)
{
  const string cgroup =
    containerizer::paths::getCgroupPath(flags.cgroups_root, container.id);

  // We remove the container so that we don't attempt multiple
  // destroys simultaneously and no other functions will return
  // information about the container that is currently being (or has
  // been) destroyed. This implies, however, that if the destroy fails
  // the caller won't be able to retry because we won't know about the
  // container anymore.
  //
  // NOTE: it's safe to use `container->id` from here on because it's
  // a copy of the Container that we're about to delete.
  containers.erase(container.id);

  // Determine if this is a partially destroyed container. A container
  // is considered partially destroyed if we have recovered it from
  // ContainerState but we don't have a freezer cgroup for it. If this
  // is a partially destroyed container than there is nothing to do.
  if (!cgroups::exists(*cgroupsInfo.freezerHierarchy, cgroup)) {
    LOG(WARNING) << "Couldn't find freezer cgroup for container "
                 << container.id << " so assuming partially destroyed";

    return _destroyCgroups(container);
  }

  LOG(INFO) << "Destroying cgroup"
               " '" << path::join(*cgroupsInfo.freezerHierarchy, cgroup) << "'";

  // TODO(benh): If this is the last container at a nesting level,
  // should we also delete the `CGROUP_SEPARATOR` cgroup too?

  // TODO(benh): What if we fail to destroy the container? Should we
  // retry?
  return cgroups::destroy(
      *cgroupsInfo.freezerHierarchy,
      cgroup,
      flags.cgroups_destroy_timeout)
    .then(defer(self(), &LinuxLauncherProcess::_destroyCgroups, container));
}


Future<Nothing> LinuxLauncherProcess::_destroyCgroups(
  const Container& container)
{
  if (cgroupsInfo.systemdHierarchy.isNone()) {
    return Nothing();
  }

  const string cgroup =
    containerizer::paths::getCgroupPath(flags.cgroups_root, container.id);

  if (!cgroups::exists(*cgroupsInfo.systemdHierarchy, cgroup)) {
    return Nothing();
  }

  LOG(INFO) << "Destroying cgroup"
               " '" << path::join(*cgroupsInfo.systemdHierarchy, cgroup) << "'";

  return cgroups::destroy(
      *cgroupsInfo.systemdHierarchy,
      cgroup,
      flags.cgroups_destroy_timeout);
}


Future<Nothing> LinuxLauncherProcess::destroyCgroups2(
  const Container& container)
{
  const string& cgroup =
    containerizer::paths::cgroups2::container(flags.cgroups_root, container.id);

  containers.erase(container.id);

  LOG(INFO) << "Destroying cgroup '" << cgroup << "'";

  return cgroups2::destroy(cgroup);
}


Future<ContainerStatus> LinuxLauncherProcess::status(
    const ContainerID& containerId)
{
  Option<Container> container = containers.get(containerId);
  if (container.isNone()) {
    return Failure("Container does not exist");
  }

  ContainerStatus status;
  if (container->pid.isSome()) {
    status.set_executor_pid(container->pid.get());
  }

  return status;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
