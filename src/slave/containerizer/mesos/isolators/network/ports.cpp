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

#include "slave/containerizer/mesos/linux_launcher.hpp"

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
    string cgroup = LinuxLauncher::cgroup(cgroupsRoot, containerId);

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

        listeners[containerId].add(address.port);
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


Try<Isolator*> NetworkPortsIsolatorProcess::create(const Flags& flags)
{
  if (flags.launcher != "linux") {
    return Error("The 'network/ports' isolator requires the 'linux' launcher");
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

  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
      new NetworkPortsIsolatorProcess(
          flags.container_ports_watch_interval,
          flags.cgroups_root,
          freezerHierarchy.get())));
}


NetworkPortsIsolatorProcess::NetworkPortsIsolatorProcess(
    const Duration& _watchInterval,
    const string& _cgroupsRoot,
    const string& _freezerHierarchy)
  : ProcessBase(process::ID::generate("network-ports-isolator")),
    watchInterval(_watchInterval),
    cgroupsRoot(_cgroupsRoot),
    freezerHierarchy(_freezerHierarchy)
{
}


bool NetworkPortsIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Nothing> NetworkPortsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const auto& state, states) {
    CHECK(!infos.contains(state.container_id()))
      << "Duplicate ContainerID " << state.container_id();

    infos.emplace(state.container_id(), Owned<Info>(new Info()));
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> NetworkPortsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // TODO(jpeach) Figure out how to ignore tasks that are not going to use the
  // host network. If they are in a network namespace (CNI network) then
  // there's no point restricting them and we would have to implement any
  // restructions by entering the right namespaces anyway.

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
    const Resources& resources)
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
    CHECK(resources.empty());

    // Verify that we know about the root for this container.
    CHECK(infos.contains(protobuf::getRootContainerId(containerId)));

    return Nothing();
  }

  Option<Value::Ranges> ports = resources.ports();
  if (ports.isSome()) {
    const Owned<Info>& info = infos.at(containerId);
    info->ports = rangesToIntervalSet<uint16_t>(ports.get()).get();
  } else {
    info->ports = IntervalSet<uint16_t>();
  }

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

    if (info->ports.isSome() && !info->ports->contains(ports)) {
      const IntervalSet<uint16_t> unallocatedPorts = ports - info->ports.get();

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

      infos.at(containerId)->limitation.set(
          protobuf::slave::createContainerLimitation(
              Resources(resource),
              message,
              TaskStatus::REASON_CONTAINER_LIMITATION));
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
            infos.keys())
          .then(defer(self, &NetworkPortsIsolatorProcess::check, lambda::_1))
          .then([]() -> ControlFlow<Nothing> { return Continue(); });
      });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
