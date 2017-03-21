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

constexpr char PID_FILE[] = "pid";
constexpr char STATUS_FILE[] = "status";
constexpr char TERMINATION_FILE[] = "termination";
constexpr char SOCKET_FILE[] = "socket";
constexpr char FORCE_DESTROY_ON_RECOVERY_FILE[] = "force_destroy_on_recovery";
constexpr char IO_SWITCHBOARD_DIRECTORY[] = "io_switchboard";
constexpr char CONTAINER_DIRECTORY[] = "containers";


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


// The containerizer uses the runtime directory (flag 'runtime_dir')
// to checkpoint things for each container, e.g., the PID of the first
// process executed within a container (i.e., the "PID 1") gets
// checkpointed in a file called 'pid'. The following helper function
// constructs the path for a container given the 'runtimeDir' that was
// used as well as the container `containerId`. For example, given two
// containers, one with ID 'a9dd' and one nested within 'a9dd' with ID
// '4e3a' and with the flag 'runtime_dir' set to '/var/run/mesos' you
// would have a directory structure that looks like:
//
// /var/run/mesos/containers/a9dd
// /var/run/mesos/containers/a9dd/pid
// /var/run/mesos/containers/a9dd/containers/4e3a/pid
std::string getRuntimePath(
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


// The helper method to read the io switchboard socket file.
Result<process::network::unix::Address> getContainerIOSwitchboardAddress(
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


// The helper method to list all container IDs (including nested
// containers) from the container runtime directory. The order of
// returned vector is a result of pre-ordering walk (i.e., parent
// is inserted before its children).
Try<std::vector<ContainerID>> getContainerIds(
    const std::string& runtimeDir);


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

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_PATHS_HPP__
