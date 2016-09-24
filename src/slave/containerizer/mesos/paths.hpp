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

#include <string>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace containerizer {
namespace paths {

constexpr char PID_FILE[] = "pid";
constexpr char STATUS_FILE[] = "status";
constexpr char CONTAINER_DIRECTORY[] = "containers";


// Returns a path representation of a ContainerID that can be used for
// creating cgroups or writing to the filesystem. A ContainerID can
// represent a nested container (i.e, it has a parent ContainerID) and
// the path representation includes all of the parents as directories
// in the path. The `prefix` parameter is prepended to each
// ContainerID as we build the path. For example, given two
// containers, one with ID 'a9dd' and one nested within 'a9dd' with ID
// '4e3a' and a prefix of 'foo' we'd get: 'foo/a9dd/foo/4e3a').
std::string buildPath(
    const ContainerID& containerId,
    const std::string& prefix = "");


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

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_PATHS_HPP__
