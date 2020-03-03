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
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using namespace process;

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
      const string& freezerHierarchy,
      const Option<string>& systemdHierarchy);

  virtual process::Future<hashset<ContainerID>> recover(
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

  virtual process::Future<Nothing> destroy(const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
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

  Future<Nothing> _destroy(const ContainerID& containerId);

  static const string subsystem;
  const Flags flags;
  const string freezerHierarchy;
  const Option<string> systemdHierarchy;
  hashmap<ContainerID, Container> containers;
};


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
  } else if (subsystems->size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        freezerHierarchy.get());
  }

  LOG(INFO) << "Using " << freezerHierarchy.get()
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
    if (!cgroups::exists(systemdHierarchy.get(), flags.cgroups_root)) {
      Try<Nothing> create = cgroups::create(
          systemdHierarchy.get(),
          flags.cgroups_root);

      if (create.isError()) {
        return Error(
            "Failed to create cgroup root under systemd hierarchy: " +
            create.error());
      }
    }

    LOG(INFO) << "Using " << systemdHierarchy.get()
              << " as the systemd hierarchy for the Linux launcher";
  }

  return new LinuxLauncher(
      flags,
      freezerHierarchy.get(),
      systemdHierarchy);
}


bool LinuxLauncher::available()
{
  // Make sure:
  //   1. Are running as root.
  //   2. 'freezer' subsystem is enabled.
  Try<bool> freezer = cgroups::enabled("freezer");
  return ::geteuid() == 0 && freezer.isSome() && freezer.get();
}


LinuxLauncher::LinuxLauncher(
    const Flags& flags,
    const string& freezerHierarchy,
    const Option<string>& systemdHierarchy)
  : process(new LinuxLauncherProcess(flags, freezerHierarchy, systemdHierarchy))
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


// `_systemdHierarchy` is only set if running on a systemd environment.
LinuxLauncherProcess::LinuxLauncherProcess(
    const Flags& _flags,
    const string& _freezerHierarchy,
    const Option<string>& _systemdHierarchy)
  : flags(_flags),
    freezerHierarchy(_freezerHierarchy),
    systemdHierarchy(_systemdHierarchy) {}


Future<hashset<ContainerID>> LinuxLauncherProcess::recover(
    const vector<ContainerState>& states)
{
  LOG(INFO) << "Recovering Linux launcher";

  // Recover all of the "containers" we know about based on the
  // existing cgroups. Note that we check both the freezer hierarchy
  // and the systemd hierarchy (if enabled), and combine the results.
  hashset<string> cgroups;

  Try<vector<string>> freezerCgroups =
    cgroups::get(freezerHierarchy, flags.cgroups_root);

  if (freezerCgroups.isError()) {
    return Failure(
        "Failed to get cgroups from " +
        path::join(freezerHierarchy, flags.cgroups_root) +
        ": "+ freezerCgroups.error());
  }

  foreach (const string& cgroup, freezerCgroups.get()) {
    cgroups.insert(cgroup);
  }

  if (systemdHierarchy.isSome()) {
    Try<vector<string>> systemdCgroups =
      cgroups::get(systemdHierarchy.get(), flags.cgroups_root);

    if (systemdCgroups.isError()) {
      return Failure(
          "Failed to get cgroups from " +
          path::join(systemdHierarchy.get(), flags.cgroups_root) +
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

  // Now loop through the containers expected by ContainerState so we
  // can have a complete list of the containers we might ever want to
  // destroy as well as be able to determine orphans below.
  hashset<ContainerID> expected = {};

  foreach (const ContainerState& state, states) {
    expected.insert(state.container_id());

    if (!containers.contains(state.container_id())) {
      // The fact that we did not have a freezer (or systemd) cgroup
      // for this container implies this container has already been
      // destroyed but we need to add it to `containers` so that when
      // `LinuxLauncher::destroy` does get called below for this
      // container we will not fail.
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
  if (systemdHierarchy.isSome()) {
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
        strings::startsWith(cgroup.get(), flags.cgroups_root);

      bool inMesosExecutorSlice =
        strings::contains(cgroup.get(), systemd::mesos::MESOS_EXECUTORS_SLICE);

      if (!inMesosCgroupRoot && !inMesosExecutorSlice) {
        LOG(WARNING)
          << "Couldn't find pid " << pid << " in either Mesos cgroup root '"
          << flags.cgroups_root << "' under systemd hierarchy, or systemd "
          << "slice '" << systemd::mesos::MESOS_EXECUTORS_SLICE << "'; "
          << "This can lead to lack of proper resource isolation";
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

  Option<pid_t> target = None();

  // Ensure nested containers have known parents.
  if (containerId.has_parent()) {
    Option<Container> container = containers.get(containerId.parent());
    if (container.isNone()) {
      return Error("Unknown parent container");
    }

    if (container->pid.isNone()) {
      // TODO(benh): Could also look up a pid in the container and use
      // that in order to enter the namespaces? This would be best
      // effort because we don't know the namespaces that had been
      // created for the original pid.
      return Error("Unknown parent container pid, can not enter namespaces");
    }

    target = container->pid.get();
  }

  // Ensure we didn't pass `enterNamespaces`
  // if we aren't forking a nested container.
  if (!containerId.has_parent() && enterNamespaces.isSome()) {
    return Error("Cannot enter parent namespaces for non-nested container");
  }

  int enterFlags = enterNamespaces.isSome() ? enterNamespaces.get() : 0;

  int cloneFlags = cloneNamespaces.isSome() ? cloneNamespaces.get() : 0;

  LOG(INFO) << "Launching " << (target.isSome() ? "nested " : "")
            << "container " << containerId << " and cloning with namespaces "
            << ns::stringify(cloneFlags);

  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  // The ordering of the hooks is:
  // (1) Create the freezer cgroup, and add the child to the cgroup.
  // (2) Create the systemd cgroup, and add the child to the cgroup.
  //
  // NOTE: The order is important here because the destroy code will
  // always kill the container based on the pids in the freezer
  // cgroup. The systemd cgroup will be removed after that. Therefore,
  // we want to make sure that if the pid is in the systemd cgroup, it
  // must be in the freezer cgroup.
  vector<Subprocess::ParentHook> parentHooks;

  // Hook for creating and assigning the child into a freezer cgroup.
  parentHooks.emplace_back(Subprocess::ParentHook([=](pid_t child) {
    return cgroups::isolate(
        freezerHierarchy,
        containerizer::paths::getCgroupPath(
            this->flags.cgroups_root,
            containerId),
        child);
  }));

  // Hook for creating and assigning the child into a systemd cgroup.
  if (systemdHierarchy.isSome()) {
    parentHooks.emplace_back(Subprocess::ParentHook([=](pid_t child) {
      return cgroups::isolate(
          systemdHierarchy.get(),
          containerizer::paths::getCgroupPath(
              this->flags.cgroups_root,
              containerId),
          child);
    }));
  }

  vector<Subprocess::ChildHook> childHooks;

  childHooks.push_back(Subprocess::ChildHook::SETSID());

  Try<Subprocess> child = subprocess(
      path,
      argv,
      containerIO.in,
      containerIO.out,
      containerIO.err,
      flags,
      environment,
      [target, enterFlags, cloneFlags](const lambda::function<int()>& child) {
        if (target.isSome()) {
          Try<pid_t> pid = ns::clone(
              target.get(),
              enterFlags,
              child,
              cloneFlags);
          if (pid.isError()) {
            LOG(WARNING) << "Failed to enter namespaces and clone: "
                         << pid.error();
            return -1;
          }
          return pid.get();
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

  const string cgroup =
    containerizer::paths::getCgroupPath(flags.cgroups_root, container->id);

  // We remove the container so that we don't attempt multiple
  // destroys simultaneously and no other functions will return
  // information about the container that is currently being (or has
  // been) destroyed. This implies, however, that if the destroy fails
  // the caller won't be able to retry because we won't know about the
  // container anymore.
  //
  // NOTE: it's safe to use `container->id` from here on because it's
  // a copy of the Container that we're about to delete.
  containers.erase(container->id);

  // Determine if this is a partially destroyed container. A container
  // is considered partially destroyed if we have recovered it from
  // ContainerState but we don't have a freezer cgroup for it. If this
  // is a partially destroyed container than there is nothing to do.
  if (!cgroups::exists(freezerHierarchy, cgroup)) {
    LOG(WARNING) << "Couldn't find freezer cgroup for container "
                 << container->id << " so assuming partially destroyed";

    return _destroy(containerId);
  }

  LOG(INFO) << "Destroying cgroup '"
            << path::join(freezerHierarchy, cgroup) << "'";

  // TODO(benh): If this is the last container at a nesting level,
  // should we also delete the `CGROUP_SEPARATOR` cgroup too?

  // TODO(benh): What if we fail to destroy the container? Should we
  // retry?
  return cgroups::destroy(
      freezerHierarchy,
      cgroup,
      flags.cgroups_destroy_timeout)
    .then(defer(
        self(),
        &LinuxLauncherProcess::_destroy,
        containerId));
}


Future<Nothing> LinuxLauncherProcess::_destroy(const ContainerID& containerId)
{
  if (systemdHierarchy.isNone()) {
    return Nothing();
  }

  const string cgroup =
    containerizer::paths::getCgroupPath(flags.cgroups_root, containerId);

  if (!cgroups::exists(systemdHierarchy.get(), cgroup)) {
    return Nothing();
  }

  LOG(INFO) << "Destroying cgroup '"
            << path::join(systemdHierarchy.get(), cgroup) << "'";

  return cgroups::destroy(
      systemdHierarchy.get(),
      cgroup,
      flags.cgroups_destroy_timeout);
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
