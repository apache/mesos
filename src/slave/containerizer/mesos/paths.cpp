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

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "slave/containerizer/mesos/paths.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace containerizer {
namespace paths {

string buildPath(
    const ContainerID& containerId,
    const string& prefix)
{
  if (!containerId.has_parent()) {
    return path::join(prefix, containerId.value());
  } else {
    return path::join(
        buildPath(containerId.parent(), prefix),
        prefix,
        containerId.value());
  }
}


string getRuntimePath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      runtimeDir,
      buildPath(containerId, CONTAINER_DIRECTORY));
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

  Try<string> read = os::read(path);
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

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
