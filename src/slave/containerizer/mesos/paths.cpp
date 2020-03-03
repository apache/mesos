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

#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>

#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "slave/state.hpp"

#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerTermination;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace containerizer {
namespace paths {

string buildPath(
    const ContainerID& containerId,
    const string& separator,
    const Mode& mode)
{
  if (!containerId.has_parent()) {
    switch (mode) {
      case PREFIX:  return path::join(separator, containerId.value());
      case SUFFIX:  return path::join(containerId.value(), separator);
      case JOIN:    return containerId.value();
      default:      UNREACHABLE();
    }
  } else {
    const string path = buildPath(containerId.parent(), separator, mode);

    switch (mode) {
      case PREFIX:  return path::join(path, separator, containerId.value());
      case SUFFIX:  return path::join(path, containerId.value(), separator);
      case JOIN:    return path::join(path, separator, containerId.value());
      default:      UNREACHABLE();
    }
  }
}


string getRuntimePath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      runtimeDir,
      buildPath(containerId, CONTAINER_DIRECTORY, PREFIX));
}


string getContainerDevicesPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      CONTAINER_DEVICES_DIRECTORY);
}


Result<pid_t> getContainerPid(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      PID_FILE);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'pid' file and thus we might
    // terminate/restart after we've created the directory but
    // before we've written the file.
    return None();
  }

  Result<string> read = state::read<string>(path);
  if (read.isError()) {
    return Error("Failed to recover pid of container: " + read.error());
  }

  Try<pid_t> pid = numify<pid_t>(read.get());
  if (pid.isError()) {
    return Error(
        "Failed to numify pid '" + read.get() +
        "' of container at '" + path + "': " + pid.error());
  }

  return pid.get();
}


Result<int> getContainerStatus(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      STATUS_FILE);

  if (!os::exists(path)) {
    return None();
  }

  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error("Unable to read status for container '" +
                 containerId.value() + "' from checkpoint file '" +
                 path + "': " + read.error());
  }

  if (read.get() != "") {
    Try<int> containerStatus = numify<int>(read.get());
    if (containerStatus.isError()) {
      return Error("Unable to read status for container '" +
                   containerId.value() + "' as integer from '" +
                   path + "': " + read.error());
    }

    return containerStatus.get();
  }

  return None();
}


#ifndef __WINDOWS__
string getContainerIOSwitchboardPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      IO_SWITCHBOARD_DIRECTORY);
}


string getContainerIOSwitchboardPidPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getContainerIOSwitchboardPath(runtimeDir, containerId),
      PID_FILE);
}


Result<pid_t> getContainerIOSwitchboardPid(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = getContainerIOSwitchboardPidPath(
      runtimeDir, containerId);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'pid' file and thus we might
    // terminate/restart after we've created the directory but
    // before we've written the file.
    return None();
  }

  Result<string> read = state::read<string>(path);
  if (read.isError()) {
    return Error("Failed to recover pid of io switchboard: " + read.error());
  }

  Try<pid_t> pid = numify<pid_t>(read.get());
  if (pid.isError()) {
    return Error(
        "Failed to numify pid '" + read.get() +
        "' of io switchboard at '" + path + "': " + pid.error());
  }

  return pid.get();
}


string getContainerIOSwitchboardSocketPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getContainerIOSwitchboardPath(runtimeDir, containerId),
      SOCKET_FILE);
}


string getContainerIOSwitchboardSocketProvisionalPath(
    const std::string& socketPath)
{
  return socketPath + "_provisional";
}


string getContainerIOSwitchboardSocketProvisionalPath(
    const std::string& runtimeDir,
    const ContainerID& containerId)
{
  return getContainerIOSwitchboardSocketProvisionalPath(
      getContainerIOSwitchboardSocketPath(runtimeDir, containerId));
}


Result<unix::Address> getContainerIOSwitchboardAddress(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = getContainerIOSwitchboardSocketPath(
      runtimeDir, containerId);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'IO_SWITCHBOARD_SOCKET_FILE' file and
    // thus we might terminate/restart after we've created the
    // directory but before we've written the file.
    return None();
  }

  Result<string> read = state::read<string>(path);
  if (read.isError()) {
    return Error("Failed reading '" + path + "': " + read.error());
  }

  Try<unix::Address> address = unix::Address::create(read.get());
  if (address.isError()) {
    return Error("Invalid AF_UNIX address: " + address.error());
  }

  return address.get();
}


string getHostProcMountPointPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      MNT_DIRECTORY,
      MNT_HOST_PROC);
}
#endif // __WINDOWS__


string getContainerForceDestroyOnRecoveryPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      FORCE_DESTROY_ON_RECOVERY_FILE);
}


bool getContainerForceDestroyOnRecovery(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = getContainerForceDestroyOnRecoveryPath(
      runtimeDir, containerId);

  if (os::exists(path)) {
    return true;
  }

  return false;
}


Result<ContainerTermination> getContainerTermination(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      TERMINATION_FILE);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'TERMINATION' file and thus we might
    // terminate/restart after we've created the directory but
    // before we've written the file.
    return None();
  }

  Result<ContainerTermination> termination =
    state::read<ContainerTermination>(path);

  if (termination.isError()) {
    return Error("Failed to read termination state of container: " +
                 termination.error());
  }

  return termination;
}


string getStandaloneContainerMarkerPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      STANDALONE_MARKER_FILE);
}


bool isStandaloneContainer(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = getStandaloneContainerMarkerPath(runtimeDir, containerId);

  return os::exists(path);
}


Result<ContainerConfig> getContainerConfig(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      CONTAINER_CONFIG_FILE);

  if (!os::exists(path)) {
    // This is possible if we recovered a container launched before we
    // started to checkpoint `ContainerConfig`.
    VLOG(1) << "Config path '" << path << "' is missing for container' "
            << containerId << "'";
    return None();
  }

  Result<ContainerConfig> containerConfig = state::read<ContainerConfig>(path);

  if (containerConfig.isError()) {
    return Error("Failed to read launch config of container: " +
                 containerConfig.error());
  }

  return containerConfig;
}


Try<vector<ContainerID>> getContainerIds(const string& runtimeDir)
{
  lambda::function<Try<vector<ContainerID>>(const Option<ContainerID>&)> helper;

  helper = [&helper, &runtimeDir](const Option<ContainerID>& parentContainerId)
    -> Try<vector<ContainerID>> {
    // Loop through each container at the path, if it exists.
    const string path = path::join(
        parentContainerId.isSome()
          ? getRuntimePath(runtimeDir, parentContainerId.get())
          : runtimeDir,
        CONTAINER_DIRECTORY);

    if (!os::exists(path)) {
      return vector<ContainerID>();
    }

    Try<list<string>> entries = os::ls(path);
    if (entries.isError()) {
      return Error("Failed to list '" + path + "': " + entries.error());
    }

    // The order always guarantee that a parent container is inserted
    // before its child containers. This is necessary for constructing
    // the hashmap 'containers_' in 'Containerizer::recover()'.
    vector<ContainerID> containers;

    foreach (const string& entry, entries.get()) {
      // We're not expecting anything else but directories here
      // representing each container.
      CHECK(os::stat::isdir(path::join(path, entry)));

      // TODO(benh): Validate that the entry looks like a ContainerID?
      ContainerID container;
      container.set_value(entry);

      if (parentContainerId.isSome()) {
        container.mutable_parent()->CopyFrom(parentContainerId.get());
      }

      containers.push_back(container);

      // Now recursively build the list of nested containers.
      Try<vector<ContainerID>> children = helper(container);
      if (children.isError()) {
        return Error(children.error());
      }

      if (!children->empty()) {
        containers.insert(containers.end(), children->begin(), children->end());
      }
    }

    return containers;
  };

  return helper(None());
}


string getContainerLaunchInfoPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      CONTAINER_LAUNCH_INFO_FILE);
}


Result<ContainerLaunchInfo> getContainerLaunchInfo(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = getContainerLaunchInfoPath(
      runtimeDir, containerId);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'CONTAINER_LAUNCH_INFO_FILE' file
    // and thus we might terminate/restart after we've created
    // the directory but before we've written the file.
    return None();
  }

  Result<ContainerLaunchInfo> containerLaunchInfo =
    state::read<ContainerLaunchInfo>(path);

  if (containerLaunchInfo.isError()) {
    return Error(
        "Failed to read ContainerLaunchInfo: " + containerLaunchInfo.error());
  }

  return containerLaunchInfo;
}


string getSandboxPath(
    const string& rootSandboxPath,
    const ContainerID& containerId)
{
  return containerId.has_parent()
    ? path::join(
        getSandboxPath(rootSandboxPath, containerId.parent()),
        "containers",
        containerId.value())
    : rootSandboxPath;
}


Try<ContainerID> parseSandboxPath(
    const ContainerID& rootContainerId,
    const string& _rootSandboxPath,
    const string& path)
{
  // Make sure there's a separator at the end of the `rootdir` so that
  // we don't accidentally slice off part of a directory.
  const string rootSandboxPath = path::join(_rootSandboxPath, "");

  if (!strings::startsWith(path, rootSandboxPath)) {
    return Error(
        "Directory '" + path + "' does not fall under "
        "the root sandbox directory '" + rootSandboxPath + "'");
  }

  ContainerID currentContainerId = rootContainerId;

  vector<string> tokens = strings::tokenize(
      path.substr(rootSandboxPath.size()),
      "/");

  for (size_t i = 0; i < tokens.size(); i++) {
    // For a nested container x.y.z, the sandbox layout is the
    // following: '.../runs/x/containers/y/containers/z'.
    if (i % 2 == 0) {
      if (tokens[i] != CONTAINER_DIRECTORY) {
        break;
      }
    } else {
      ContainerID id;
      id.set_value(tokens[i]);
      id.mutable_parent()->CopyFrom(currentContainerId);
      currentContainerId = id;
    }
  }

  return currentContainerId;
}


string getContainerShmPath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePath(runtimeDir, containerId),
      CONTAINER_SHM_DIRECTORY);
}


Try<string> getParentShmPath(
    const string runtimeDir,
    const ContainerID& containerId)
{
  CHECK(containerId.has_parent());

  ContainerID parentId = containerId.parent();

  Result<ContainerConfig> parentConfig =
    getContainerConfig(runtimeDir, parentId);

  if (parentConfig.isNone()) {
    return Error(
        "Failed to find config for container " + stringify(parentId));
  } else if (parentConfig.isError()) {
    return Error(parentConfig.error());
  }

  string parentShmPath;

  if (parentConfig->has_container_info() &&
      parentConfig->container_info().has_linux_info() &&
      parentConfig->container_info().linux_info().has_ipc_mode()) {
    switch (parentConfig->container_info().linux_info().ipc_mode()) {
      case LinuxInfo::PRIVATE: {
        const string shmPath = getContainerShmPath(runtimeDir, parentId);
        if (!os::exists(shmPath)) {
          return Error(
              "The shared memory path '" + shmPath + "' of container "
              + stringify(parentId) + " does not exist");
        }

        parentShmPath = shmPath;
        break;
      }
      case LinuxInfo::SHARE_PARENT: {
        if (parentId.has_parent()) {
          return getParentShmPath(runtimeDir, parentId);
        }

        parentShmPath = AGENT_SHM_DIRECTORY;
        break;
      }
      case LinuxInfo::UNKNOWN: {
        LOG(FATAL) << "The IPC mode of container " << parentId << " is UNKNOWN";
      }
    }
  } else {
    if (parentConfig->has_rootfs()) {
      return Error(
          "The shared memory of container " + stringify(parentId) +
          " cannot be shared with any other containers because it"
          " is only in the container's own mount namespace");
    }

    parentShmPath = AGENT_SHM_DIRECTORY;
  }

  return parentShmPath;
}


string getCgroupPath(
    const string& cgroupsRoot,
    const ContainerID& containerId)
{
  return path::join(
      cgroupsRoot,
      containerizer::paths::buildPath(
          containerId,
          CGROUP_SEPARATOR,
          containerizer::paths::JOIN));
}


Option<ContainerID> parseCgroupPath(
    const string& cgroupsRoot,
    const string& cgroup)
{
  Option<ContainerID> current;

  // Start not expecting to see a separator and adjust after each
  // non-separator we see.
  bool separator = false;

  vector<string> tokens = strings::tokenize(
      strings::remove(cgroup, cgroupsRoot, strings::PREFIX),
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

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
