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

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

static const char CGROUP_SEPARATOR[] = "mesos";


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
      const list<mesos::slave::ContainerState>& states);

  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const string& path,
      const vector<string>& argv,
      const process::Subprocess::IO& in,
      const process::Subprocess::IO& out,
      const process::Subprocess::IO& err,
      const flags::FlagsBase* flags,
      const Option<map<string, string>>& environment,
      const Option<int>& enterNamespaces,
      const Option<int>& cloneNamespaces);

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

  // Helper for determining the cgroup for a container (i.e., the path
  // in a cgroup subsystem).
  string cgroup(const ContainerID& containerId);

  // Helper for parsing the cgroup path to determine the container ID
  // it belongs to.
  Option<ContainerID> parse(const string& cgroup);

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
  //
  // The LinuxLauncherProcess takes responsibility for creating and
  // starting this slice. It then migrates executor pids into this
  // slice before it "unpauses" the executor. This is the same pattern
  // as the freezer.

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
  //   1. Are running as root.
  //   2. 'freezer' subsytem is enabled.
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
    const list<mesos::slave::ContainerState>& states)
{
  return dispatch(process.get(), &LinuxLauncherProcess::recover, states);
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
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces)
{
  return dispatch(
      process.get(),
      &LinuxLauncherProcess::fork,
      containerId,
      path,
      argv,
      in,
      out,
      err,
      flags,
      environment,
      enterNamespaces,
      cloneNamespaces).get();
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
    const list<ContainerState>& states)
{
  // Recover all of the "containers" we know about based on the
  // existing cgroups.
  Try<vector<string>> cgroups =
    cgroups::get(freezerHierarchy, flags.cgroups_root);

  if (cgroups.isError()) {
    return Failure(
        "Failed to get cgroups from " +
        path::join(freezerHierarchy, flags.cgroups_root) +
        ": "+ cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Need to parse the cgroup to see if it's one we created (i.e.,
    // matches our separator structure) or one that someone else
    // created (e.g., in the future we might have nested containers
    // that are managed by something else rooted within the freezer
    // hierarchy).
    Option<ContainerID> containerId = parse(cgroup);
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
      // The fact that we did not have a freezer cgroup for this
      // container implies this container has already been destroyed
      // but we need to add it to `containers` so that when
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
  // still in the `MESOS_EXECUTORS_SLICE`. If they are not, warn the
  // operator that resource isolation may be invalidated.
  if (systemdHierarchy.isSome()) {
    Result<set<pid_t>> mesosExecutorSlicePids = cgroups::processes(
        systemdHierarchy.get(),
        systemd::mesos::MESOS_EXECUTORS_SLICE);

    // If we error out trying to read the pids from the
    // `MESOS_EXECUTORS_SLICE` we fail. This is a programmer error
    // as we did not set up the slice correctly.
    if (mesosExecutorSlicePids.isError()) {
      return Failure("Failed to read pids from systemd '" +
                     stringify(systemd::mesos::MESOS_EXECUTORS_SLICE) + "'");
    }

    if (mesosExecutorSlicePids.isSome()) {
      foreachvalue (const Container& container, containers) {
        if (container.pid.isNone()) {
          continue;
        }

        if (mesosExecutorSlicePids.get().count(container.pid.get()) != 0) {
          // TODO(jmlvanre): Add a flag that enforces this rather
          // than just logs a warning (i.e., we exit if a pid was
          // found in the freezer but not in the
          // `MESOS_EXECUTORS_SLICE`. We need a flag to support the
          // upgrade path.
          LOG(WARNING)
            << "Couldn't find pid '" << container.pid.get() << "' in '"
            << systemd::mesos::MESOS_EXECUTORS_SLICE << "'. This can lead to"
            << " lack of proper resource isolation";
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


Try<pid_t> LinuxLauncherProcess::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const process::Subprocess::IO& in,
    const process::Subprocess::IO& out,
    const process::Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces)
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
  // if we aren't forking a nested contiainer.
  if (!containerId.has_parent() && enterNamespaces.isSome()) {
    return Error("Cannot enter parent namespaces for non-nested container");
  }

  int enterFlags = enterNamespaces.isSome() ? enterNamespaces.get() : 0;

  int cloneFlags = cloneNamespaces.isSome() ? cloneNamespaces.get() : 0;

  LOG(INFO) << "Launching " << (target.isSome() ? "nested " : "")
            << "container " << containerId << " and cloning with namespaces "
            << ns::stringify(cloneFlags);

  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  // NOTE: The ordering of the hooks is:
  //
  // (1) Add the child to the systemd slice.
  // (2) Create the freezer cgroup for the child.
  //
  // But since both have to happen or the child will terminate the
  // ordering is immaterial.

  // Hook to extend the life of the child (and all of its
  // descendants) using a systemd slice.
  vector<Subprocess::ParentHook> parentHooks;

  if (systemdHierarchy.isSome()) {
    parentHooks.emplace_back(Subprocess::ParentHook([](pid_t child) {
      return systemd::mesos::extendLifetime(child);
    }));
  }

  // Hook for creating and assigning the child into a freezer cgroup.
  parentHooks.emplace_back(Subprocess::ParentHook([=](pid_t child) {
    return cgroups::isolate(
        freezerHierarchy,
        cgroup(containerId),
        child);
  }));

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
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
      {Subprocess::ChildHook::SETSID()});

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
  }

  Container container;
  container.id = containerId;
  container.pid = child.get().pid();

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
  Try<bool> exists = cgroups::exists(freezerHierarchy, cgroup(container->id));
  if (exists.isError()) {
    return Failure("Failed to determine if cgroup exists: " + exists.error());
  }

  if (!exists.get()) {
    LOG(WARNING) << "Couldn't find freezer cgroup for container "
                 << container->id << " so assuming partially destroyed";
    return Nothing();
  }

  LOG(INFO) << "Using freezer to destroy cgroup " << cgroup(container->id);

  // TODO(benh): If this is the last container at a nesting level,
  // should we also delete the `CGROUP_SEPARATOR` cgroup too?

  // TODO(benh): What if we fail to destroy the container? Should we
  // retry?
  return cgroups::destroy(
      freezerHierarchy,
      cgroup(container->id),
      cgroups::DESTROY_TIMEOUT);
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


string LinuxLauncherProcess::cgroup(const ContainerID& containerId)
{
  return path::join(
      flags.cgroups_root,
      containerizer::paths::buildPath(
          containerId,
          CGROUP_SEPARATOR,
          containerizer::paths::JOIN));
}


Option<ContainerID> LinuxLauncherProcess::parse(const string& cgroup)
{
  Option<ContainerID> current;

  // Start not expecting to see a separator and adjust after each
  // non-separator we see.
  bool separator = false;

  vector<string> tokens = strings::tokenize(
      strings::remove(cgroup, flags.cgroups_root, strings::PREFIX),
      stringify(os::PATH_SEPARATOR));

  for (size_t i = 0; i < tokens.size(); i++) {
    if (separator && tokens[i] == CGROUP_SEPARATOR) {
      separator = false;

      // If the cgroup has CGROUP_SEPARATOR as the last segment,
      // should just ignore it because this cgroup belongs to us.
      if (i == tokens.size() - 1) {
        return None();
      } else {
        continue;
      }
    } else if (separator) {
      return None();
    } else {
      separator = true;
    }

    ContainerID id;
    id.set_value(tokens[i]);

    if (current.isSome()) {
      id.mutable_parent()->CopyFrom(current.get());
    }

    current = id;
  }

  return current;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
