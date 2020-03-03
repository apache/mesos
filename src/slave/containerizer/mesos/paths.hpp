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

#ifndef __MESOS_CONTAINERIZER_PATHS_HPP__
#define __MESOS_CONTAINERIZER_PATHS_HPP__

#include <sys/types.h>

#include <string>
#include <vector>

#include <process/address.hpp>

#include <stout/result.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.hpp>

#include <mesos/slave/containerizer.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace containerizer {
namespace paths {


// The containerizer uses the runtime directory to checkpoint things
// for each container, like the PID of the first process executed
// within a container (i.e., the "PID 1") or the LaunchInfo associated
// with the given container.
//
// The file system layout is as follows:
//
//   root ('--runtime_dir' flag)
//   |-- containers
//       |-- <container_id>
//           |-- config
//           |-- containers
//           |   |-- <container_id>
//           |   |   |-- <more nesting of containers>
//           |   |-- pid
//           |   |-- ...
//           |-- devices
//           |-- force_destroy_on_recovery
//           |-- io_switchboard
//           |   |-- pid
//           |   |-- socket
//           |-- launch_info
//           |-- mnt
//           |   |-- host_proc
//           |-- pid
//           |-- shm
//           |-- standalone.marker
//           |-- status
//           |-- termination


constexpr char PID_FILE[] = "pid";
constexpr char CONTAINER_CONFIG_FILE[] = "config";
constexpr char STATUS_FILE[] = "status";
constexpr char TERMINATION_FILE[] = "termination";
constexpr char SOCKET_FILE[] = "socket";
constexpr char FORCE_DESTROY_ON_RECOVERY_FILE[] = "force_destroy_on_recovery";
constexpr char IO_SWITCHBOARD_DIRECTORY[] = "io_switchboard";
constexpr char MNT_DIRECTORY[] = "mnt";
constexpr char MNT_HOST_PROC[] = "host_proc";
constexpr char CONTAINER_DIRECTORY[] = "containers";
constexpr char CONTAINER_DEVICES_DIRECTORY[] = "devices";
constexpr char CONTAINER_LAUNCH_INFO_FILE[] = "launch_info";
constexpr char STANDALONE_MARKER_FILE[] = "standalone.marker";
constexpr char CONTAINER_SHM_DIRECTORY[] = "shm";
constexpr char AGENT_SHM_DIRECTORY[] = "/dev/shm";
constexpr char SECRET_DIRECTORY[] = ".secret";


enum Mode
{
  PREFIX,
  SUFFIX,
  JOIN,
};

// Returns a path representation of a ContainerID that can be used for
// creating cgroups or writing to the filesystem. A ContainerID can
// represent a nested container (i.e, it has a parent ContainerID) and
// the path representation includes all of the parents as directories
// in the path. Depending on the 'mode', the result can be the
// following for a nested container 'xxx.yyy':
//   1) mode == PREFIX: '<separator>/xxx/<separator>/yyy'
//   2) mode == SUFFIX: 'xxx/<separator>/yyy/<separator>'
//   3) mode == JOIN:   'xxx/<separator>/yyy'
std::string buildPath(
    const ContainerID& containerId,
    const std::string& separator,
    const Mode& mode);


// The following helper function constructs the path
// for a container given the 'runtimeDir' that was
// used as well as the container `containerId`.
std::string getRuntimePath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// Given a `runtimeDir`, construct a unique directory to stage
// per-container device nodes. This directory is initially created
// and  populated by the `filesystem/linux` isolator. Any subsequent
// isolators may add devices to this directory and bind mount them
// into the container.
std::string getContainerDevicesPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to read the pid file.
Result<pid_t> getContainerPid(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to read the status file.
Result<int> getContainerStatus(
    const std::string& runtimeDir,
    const ContainerID& containerId);


#ifndef __WINDOWS__
// The helper method to get the io switchboard directory path.
std::string getContainerIOSwitchboardPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the io switchboard pid file path.
std::string getContainerIOSwitchboardPidPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the io switchboard pid.
Result<pid_t> getContainerIOSwitchboardPid(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the socket file path.
std::string getContainerIOSwitchboardSocketPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the io switchboard provisional socket path,
// see comment in IOSwitchboardServer::create().
std::string getContainerIOSwitchboardSocketProvisionalPath(
    const std::string& socketPath);


// The helper method to get the io switchboard provisional socket path,
// see comment in IOSwitchboardServer::create().
std::string getContainerIOSwitchboardSocketProvisionalPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to read the io switchboard socket file.
Result<process::network::unix::Address> getContainerIOSwitchboardAddress(
    const std::string& runtimeDir,
    const ContainerID& containerId);

// The helper method to get the host proc mount point path.
std::string getHostProcMountPointPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);
#endif


// The helper method to get the destroy on recovery file path.
std::string getContainerForceDestroyOnRecoveryPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to check if we should
// destroy a container on recovery or not.
bool getContainerForceDestroyOnRecovery(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to read the container termination state.
Result<mesos::slave::ContainerTermination> getContainerTermination(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the standalone container marker path.
std::string getStandaloneContainerMarkerPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to check if the given container is a standalone
// container or not. This is determined by the existence (or not) of
// a marker file in the container's runtime metadata directory.
bool isStandaloneContainer(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to read the launch config of the contaienr.
Result<mesos::slave::ContainerConfig> getContainerConfig(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to list all container IDs (including nested
// containers) from the container runtime directory. The order of
// returned vector is a result of pre-ordering walk (i.e., parent
// is inserted before its children).
Try<std::vector<ContainerID>> getContainerIds(
    const std::string& runtimeDir);


// The helper method to get the container launch information path.
std::string getContainerLaunchInfoPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the container launch information
// at the moment it was launched.
Result<mesos::slave::ContainerLaunchInfo> getContainerLaunchInfo(
    const std::string& runtimeDir,
    const ContainerID& containerId);


// The helper method to get the sandbox path.
std::string getSandboxPath(
    const std::string& rootSandboxPath,
    const ContainerID& containerId);


// The helper method parses a given 'path' and returns the container
// ID of the container whose sandbox contains 'path'.
Try<ContainerID> parseSandboxPath(
    const ContainerID& rootContainerId,
    const std::string& rootSandboxPath,
    const std::string& path);


std::string getContainerShmPath(
    const std::string& runtimeDir,
    const ContainerID& containerId);


Try<std::string> getParentShmPath(
    const std::string runtimeDir,
    const ContainerID& containerId);


// Helper for determining the cgroup for a container (i.e., the path
// in a cgroup subsystem).
std::string getCgroupPath(
    const std::string& cgroupsRoot,
    const ContainerID& containerId);


// Helper for parsing the cgroup path to determine the container ID
// it belongs to.
Option<ContainerID> parseCgroupPath(
    const std::string& cgroupsRoot,
    const std::string& cgroup);

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_PATHS_HPP__
