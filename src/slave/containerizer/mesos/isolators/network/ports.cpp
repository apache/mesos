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

#include "slave/containerizer/mesos/isolators/network/ports.hpp"

#include <dirent.h>

#include <sys/types.h>

#include <process/after.hpp>
#include <process/async.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>

#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>

#include "common/protobuf_utils.hpp"
#include "common/values.hpp"

#include "linux/cgroups.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/mesos/paths.hpp"

using std::list;
using std::set;
using std::string;
using std::vector;

using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::defer;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::internal::values::intervalSetToRanges;
using mesos::internal::values::rangesToIntervalSet;

using namespace routing::diagnosis;

namespace mesos {
namespace internal {
namespace slave {

// Given a cgroup hierarchy and a set of container IDs, collect
// the ports of all the listening sockets open in each cgroup,
// indexed by container ID.
static hashmap<ContainerID, IntervalSet<uint16_t>>
collectContainerListeners(
    const string& cgroupsRoot,
    const string& freezerHierarchy,
    const Option<IntervalSet<uint16_t>>& isolatedPorts,
    const hashset<ContainerID>& containerIds)
{
  hashmap<ContainerID, IntervalSet<uint16_t>> listeners;

  Try<hashmap<uint32_t, socket::Info>> listenInfos =
    NetworkPortsIsolatorProcess::getListeningSockets();

  if (listenInfos.isError()) {
    LOG(ERROR) << "Failed to query listening sockets: "
               << listenInfos.error();
    return listeners;
  }

  if (listenInfos->empty()) {
    return listeners;
  }

  foreach (const ContainerID& containerId, containerIds) {
    // Reconstruct the cgroup path from the container ID.
    string cgroup =
      containerizer::paths::getCgroupPath(cgroupsRoot, containerId);

    VLOG(1) << "Checking processes for container " << containerId
            << " in cgroup " << cgroup;

    Try<set<pid_t>> pids = cgroups::processes(freezerHierarchy, cgroup);
    if (pids.isError()) {
      LOG(ERROR) << "Failed to list processes for container "
                 << containerId << ": " << pids.error();
      continue;
    }

    // For each process in this container, check whether any of its open
    // sockets matches something in the listening set.
    foreach (pid_t pid, pids.get()) {
      Try<vector<uint32_t>> sockets =
        NetworkPortsIsolatorProcess::getProcessSockets(pid);

      // The PID might have exited since we sampled the cgroup tasks, so
      // don't worry too much if this fails.
      if (sockets.isError()) {
        VLOG(1) << "Failed to list sockets for PID "
                << stringify(pid) << ": " << sockets.error();
        continue;
      }

      if (sockets->empty()) {
        continue;
      }

      foreach (uint32_t inode, sockets.get()) {
        if (!listenInfos->contains(inode)) {
          continue;
        }

        const auto& socketInfo = listenInfos->at(inode);

        process::network::inet::Address address(
            socketInfo.sourceIP.get(),
            ntohs(socketInfo.sourcePort.get()));

        if (VLOG_IS_ON(1)) {
          Result<string> cmd = proc::cmdline(pid);
          if (cmd.isSome()) {
            VLOG(1) << "PID " << pid << " in container " << containerId
                    << " (" << cmd.get() << ")"
                    << " is listening on port " << address.port;
          } else {
            VLOG(1) << "PID " << pid << " in container " << containerId
                    << " is listening on port " << address.port;
          }
        }

        // Only collect this listen socket if it falls within the
        // isolated range.
        if (isolatedPorts.isNone() || isolatedPorts->contains(address.port)) {
          listeners[containerId].add(address.port);
        }
      }
    }
  }

  return listeners;
}


// Return a hashmap of routing::diagnosis::socket::Info structures
// for all listening sockets, indexed by the socket inode.
Try<hashmap<uint32_t, socket::Info>>
NetworkPortsIsolatorProcess::getListeningSockets()
{
  Try<vector<socket::Info>> socketInfos = socket::infos(
      AF_INET,
      socket::state::LISTEN);

  if (socketInfos.isError()) {
    return Error(socketInfos.error());
  }

  hashmap<uint32_t, socket::Info> inodes;

  foreach (const socket::Info& info, socketInfos.get()) {
    // The inode should never be 0. This would only happen if the kernel
    // didn't return the inode in the sockdiag response, which would imply
    // a very old kernel or a problem between the kernel and libnl.
    if (info.inode != 0) {
      inodes.emplace(info.inode, info);
    }
  }

  return inodes;
}


// Extract the inode field from a /proc/$PID/fd entry. The format of
// the socket entry is "socket:[nnnn]" where nnnn is the numberic inode
// number of the socket.
static uint32_t extractSocketInode(const string& sock)
{
  const size_t s = sizeof("socket:[]") - 1;
  const string val = sock.substr(s - 1, sock.size() - s);

  Try<uint32_t> value = numify<uint32_t>(val);
  CHECK_SOME(value);

  return value.get();
}


// Return the inodes of all the sockets open in the the given process.
Try<vector<uint32_t>> NetworkPortsIsolatorProcess::getProcessSockets(pid_t pid)
{
  const string fdPath = path::join("/proc", stringify(pid), "fd");

  DIR* dir = opendir(fdPath.c_str());
  if (dir == nullptr) {
    return ErrnoError("Failed to open directory '" + fdPath + "'");
  }

  vector<uint32_t> inodes;
  struct dirent* entry;
  char target[NAME_MAX];

  while (true) {
    errno = 0;
    if ((entry = readdir(dir)) == nullptr) {
      // If errno is non-zero, readdir failed.
      if (errno != 0) {
        Error error = ErrnoError("Failed to read directory '" + fdPath + "'");
        CHECK_EQ(closedir(dir), 0) << os::strerror(errno);
        return error;
      }

      // Otherwise we just reached the end of the directory and we are done.
      CHECK_EQ(closedir(dir), 0) << os::strerror(errno);
      return inodes;
    }

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    ssize_t nbytes = readlinkat(
        dirfd(dir), entry->d_name, target, sizeof(target) - 1);

    if (nbytes == -1) {
      Error error = ErrnoError(
          "Failed to read symbolic link '" +
          path::join(fdPath, entry->d_name) + "'");

      CHECK_EQ(closedir(dir), 0) << os::strerror(errno);
      return error;
    }

    target[nbytes] = '\0';

    if (strings::startsWith(target, "socket:[")) {
      inodes.push_back(extractSocketInode(target));
    }
  }
}


// Return true if any "ports" resources are specified in the flags. This
// is later used to distinguish between empty an unspecified ports.
static bool havePortsResource(const Flags& flags)
{
  vector<Resource> resourceList = Resources::fromString(
      flags.resources.getOrElse(""), flags.default_role).get();

  foreach(const auto& resource, resourceList) {
    if (resource.name() == "ports") {
      return true;
    }
  }

  return false;
}


Try<Isolator*> NetworkPortsIsolatorProcess::create(const Flags& flags)
{
  if (flags.launcher != "linux") {
    return Error("The 'network/ports' isolator requires the 'linux' launcher");
  }

  if (flags.check_agent_port_range_only &&
      flags.container_ports_isolated_range.isSome()) {
    return Error(
        "Only one of `--check_agent_port_range_only` or "
        "'--container_ports_isolated_range` should be specified");
  }

  Try<string> freezerHierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "freezer",
      flags.cgroups_root);

  if (freezerHierarchy.isError()) {
    return Error(
        "Failed to prepare the freezer cgroup: " +
        freezerHierarchy.error());
  }

  // Set None as the default of isolated ports range.
  Option<IntervalSet<uint16_t>> isolatedPorts = None();

  // If we are only watching the ports in the agent resources, figure
  // out what the agent ports will be by checking the resources flag
  // and falling back to the default.
  if (flags.check_agent_port_range_only) {
    Try<Resources> resources = Resources::parse(
        flags.resources.getOrElse(""),
        flags.default_role);

    if (resources.isError()) {
      return Error(
          "Failed to parse agent resources: " + resources.error());
    }

    // Mirroring the logic in Containerizer::resources(), we need
    // to distinguish between an empty ports resource and a missing
    // ports resource.
    if (!havePortsResource(flags)) {
      // Apply the defaults, if no ports were specified.
      resources = Resources(
          Resources::parse(
              "ports",
              stringify(DEFAULT_PORTS),
              flags.default_role).get());

      isolatedPorts =
        rangesToIntervalSet<uint16_t>(resources->ports().get()).get();
    } else if (resources->ports().isSome()) {
      // Use the given non-empty ports resource.
      Try<IntervalSet<uint16_t>> ports =
        rangesToIntervalSet<uint16_t>(resources->ports().get());

      if (ports.isError()) {
        return Error(
            "Invalid ports resource '" +
            stringify(resources->ports().get()) +
            "': " + ports.error());
      }

      isolatedPorts = ports.get();
    } else {
      // An empty ports resource was specified.
      isolatedPorts = IntervalSet<uint16_t>{};
    }
  }

  // Use the given isolated ports range if specified.
  if (flags.container_ports_isolated_range.isSome()) {
    Try<Resource> portRange =
      Resources::parse(
          "ports",
          flags.container_ports_isolated_range.get(),
          "*");

    if (portRange.isError()) {
      return Error(
          "Failed to parse isolated ports range '" +
          flags.container_ports_isolated_range.get() + "'");
    }

    if (portRange->type() != Value::RANGES) {
      return Error(
          "Invalid port range resource type " +
          mesos::Value_Type_Name(portRange->type()) +
          ", expecting " +
          mesos::Value_Type_Name(Value::RANGES));
    }

    Try<IntervalSet<uint16_t>> ports =
      rangesToIntervalSet<uint16_t>(portRange->ranges());

    if (ports.isError()) {
      return Error(ports.error());
    }

    isolatedPorts = ports.get();
  }

  if (isolatedPorts.isSome()) {
    LOG(INFO) << "Isolating port range " << stringify(isolatedPorts.get());
  }

  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
      new NetworkPortsIsolatorProcess(
          strings::contains(flags.isolation, "network/cni"),
          flags.container_ports_watch_interval,
          flags.enforce_container_ports,
          flags.cgroups_root,
          freezerHierarchy.get(),
          isolatedPorts)));
}


NetworkPortsIsolatorProcess::NetworkPortsIsolatorProcess(
    bool _cniIsolatorEnabled,
    const Duration& _watchInterval,
    const bool& _enforceContainerPorts,
    const string& _cgroupsRoot,
    const string& _freezerHierarchy,
    const Option<IntervalSet<uint16_t>>& _isolatedPorts)
  : ProcessBase(process::ID::generate("network-ports-isolator")),
    cniIsolatorEnabled(_cniIsolatorEnabled),
    watchInterval(_watchInterval),
    enforceContainerPorts(_enforceContainerPorts),
    cgroupsRoot(_cgroupsRoot),
    freezerHierarchy(_freezerHierarchy),
    isolatedPorts(_isolatedPorts)
{
}


bool NetworkPortsIsolatorProcess::supportsNesting()
{
  return true;
}


// Return whether this ContainerInfo has a NetworkInfo with a name. This
// is our signal that the container is (or will be) joined to a CNI network.
static bool hasNamedNetwork(const ContainerInfo& container_info)
{
  foreach (const auto& networkInfo, container_info.network_infos()) {
    if (networkInfo.has_name()) {
      return true;
    }
  }

  return false;
}


// Recover the given list of containers. Note that we don't look at
// the executor resources from the ContainerState because from the
// perspective of the container, they are incomplete. That is, the
// resources here are only the resources for the executor, not the
// resources for the whole container. At this point, we don't know
// whether any of the tasks in the container have been allocated ports,
// so we must not start isolating it until we receive the resources
// update.
Future<Nothing> NetworkPortsIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // First, recover all the root level containers.
  foreach (const auto& state, states) {
    if (state.container_id().has_parent()) {
      continue;
    }

    CHECK(!infos.contains(state.container_id()))
      << "Duplicate ContainerID " << state.container_id();

    // A root level container ought to always have an executor_info.
    CHECK(state.has_executor_info());

    if (!cniIsolatorEnabled) {
      infos.emplace(state.container_id(), Owned<Info>(new Info()));
      continue;
    }

    // Ignore containers that will be network isolated by the
    // `network/cni` isolator on the rationale that they ought
    // to be getting a per-container IP address.
    if (!state.executor_info().has_container() ||
        !hasNamedNetwork(state.executor_info().container())) {
      infos.emplace(state.container_id(), Owned<Info>(new Info()));
    }
  }

  // Now that we know which root level containers we are isolating, we can
  // decide which child containers we also want.
  foreach (const auto& state, states) {
    if (!state.container_id().has_parent()) {
      continue;
    }

    CHECK(!infos.contains(state.container_id()))
      << "Duplicate ContainerID " << state.container_id();

    if (infos.contains(protobuf::getRootContainerId(state.container_id()))) {
      infos.emplace(state.container_id(), Owned<Info>(new Info()));
    }
  }

  // We don't need to worry about any orphans since we don't have any state
  // to clean up and we know the containerizer will destroy them soon.

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> NetworkPortsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  if (cniIsolatorEnabled) {
    // A nested container implicitly joins a parent CNI network. The
    // network configuration is always set from the top of the tree of
    // nested containers, so we know that we should only isolate the
    // child if we already have the root of the container tree.
    if (containerId.has_parent()) {
      if (!infos.contains(protobuf::getRootContainerId(containerId))) {
        return None();
      }
    } else {
      // Ignore containers that will be network isolated by the
      // `network/cni` isolator on the rationale that they ought
      // to be getting a per-container IP address.
      if (containerConfig.has_container_info() &&
          hasNamedNetwork(containerConfig.container_info())) {
        return None();
      }
    }
  }

  infos.emplace(containerId, Owned<Info>(new Info()));

  return update(containerId, containerConfig.resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<ContainerLimitation> NetworkPortsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to watch ports for unknown container " +
        stringify(containerId));
  }

  return infos.at(containerId)->limitation.future();
}


Future<Nothing> NetworkPortsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  const Owned<Info>& info = infos.at(containerId);

  // The resources are attached to the root container. For child
  // containers, just track its existence so that we can scan
  // processes in the corresponding cgroup.
  if (containerId.has_parent()) {
    // Child containers don't get resources, only the parents do.
    CHECK(resourceRequests.empty());

    // Verify that we know about the root for this container.
    CHECK(infos.contains(protobuf::getRootContainerId(containerId)));

    return Nothing();
  }

  Option<Value::Ranges> ports = resourceRequests.ports();
  if (ports.isSome()) {
    const Owned<Info>& info = infos.at(containerId);
    info->allocatedPorts = rangesToIntervalSet<uint16_t>(ports.get()).get();
  } else {
    info->allocatedPorts = IntervalSet<uint16_t>();
  }

  LOG(INFO) << "Updated ports to "
            << intervalSetToRanges(info->allocatedPorts.get())
            << " for container " << containerId;

  return Nothing();
}


Future<Nothing> NetworkPortsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring cleanup for unknown container " << containerId;
    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


// Given a map of containers to the ports they are listening on,
// verify that each container is only listening on ports that we
// have recorded as being allocated to it.
Future<Nothing> NetworkPortsIsolatorProcess::check(
    const hashmap<ContainerID, IntervalSet<uint16_t>>& listeners)
{
  foreachpair (const ContainerID& containerId,
               const IntervalSet<uint16_t>& ports,
               listeners) {
    if (!infos.contains(containerId)) {
      continue;
    }

    ContainerID rootContainerId = protobuf::getRootContainerId(containerId);
    CHECK(infos.contains(rootContainerId));

    // Find the corresponding root container that holds the resources
    // for this container.
    const Owned<Info>& info = infos.at(rootContainerId);

    if (info->allocatedPorts.isSome() &&
        !info->allocatedPorts->contains(ports)) {
      const IntervalSet<uint16_t> unallocatedPorts =
          ports - info->allocatedPorts.get();

      // Only log unallocated ports once to prevent excessive logging
      // for the same unallocated ports while port enforcement is disabled.
      if (info->activePorts.isSome() && info->activePorts == ports) {
        continue;
      }

      // Cache the last listeners port sample so that we will
      // only log new ports resource violations.
      info->activePorts = ports;

      Resource resource;
      resource.set_name("ports");
      resource.set_type(Value::RANGES);
      resource.mutable_ranges()->CopyFrom(
          intervalSetToRanges(unallocatedPorts));

      const string message =
        "Container " + stringify(containerId) +
        " is listening on unallocated port(s): " +
        stringify(resource.ranges());

      LOG(INFO) << message;

      if (enforceContainerPorts) {
        info->limitation.set(
        protobuf::slave::createContainerLimitation(
            Resources(resource),
            message,
            TaskStatus::REASON_CONTAINER_LIMITATION));
      }
    }
  }

  return Nothing();
}


void NetworkPortsIsolatorProcess::initialize()
{
  process::PID<NetworkPortsIsolatorProcess> self(this);

  // Start a loop to periodically reconcile listening ports against allocated
  // resources. Note that we have to do this after the process we want the
  // loop to schedule against (the ports isolator process) has been spawned.
  process::loop(
      self,
      [=]() {
        return process::after(watchInterval);
      },
      [=](const Nothing&) {
        return process::async(
            &collectContainerListeners,
            cgroupsRoot,
            freezerHierarchy,
            isolatedPorts,
            infos.keys())
          .then(defer(self, &NetworkPortsIsolatorProcess::check, lambda::_1))
          .then([]() -> ControlFlow<Nothing> { return Continue(); });
      });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
