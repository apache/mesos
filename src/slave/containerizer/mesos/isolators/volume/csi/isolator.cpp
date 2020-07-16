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

#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/os.hpp>

#include <stout/os/realpath.hpp>

#include "slave/containerizer/mesos/isolators/volume/csi/isolator.hpp"
#include "slave/containerizer/mesos/isolators/volume/csi/paths.hpp"

using std::string;
using std::vector;

using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> VolumeCSIIsolatorProcess::create(
    const Flags& flags,
    CSIServer* csiServer)
{
  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' isolator must be used");
  }

  if (csiServer == nullptr) {
    return Error("No CSI server is provided");
  }

  const string csiRootDir = path::join(flags.runtime_dir, csi::paths::CSI_DIR);

  // Create the CSI volume information root directory if it does not exist.
  Try<Nothing> mkdir = os::mkdir(csiRootDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create CSI volume information root directory at '" +
        csiRootDir + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(csiRootDir);
  if (!rootDir.isSome()) {
    return Error(
        "Failed to determine canonical path of CSI volume information root"
        " directory '" + csiRootDir + "': " +
        (rootDir.isError() ? rootDir.error() : "No such file or directory"));
  }

  Owned<MesosIsolatorProcess> process(new VolumeCSIIsolatorProcess(
      flags,
      csiServer,
      rootDir.get()));

  return new MesosIsolator(process);
}


bool VolumeCSIIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Nothing> VolumeCSIIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<Option<ContainerLaunchInfo>> VolumeCSIIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return None();
}


Future<Nothing> VolumeCSIIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
