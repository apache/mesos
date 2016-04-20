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

#include <list>

#include <stout/os.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/docker/volume/isolator.hpp"

namespace paths = mesos::internal::slave::docker::volume::paths;

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::internal::slave::docker::volume::DriverClient;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerVolumeIsolatorProcess::DockerVolumeIsolatorProcess(
    const Flags& _flags,
    const string& _rootDir,
    const Owned<DriverClient>& _client)
  : flags(_flags),
    rootDir(_rootDir),
    client(_client) {}


DockerVolumeIsolatorProcess::~DockerVolumeIsolatorProcess() {}


Try<Isolator*> DockerVolumeIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The 'docker/volume' isolator requires root permissions");
  }

  // TODO(gyliu513): Check dvdcli version, the version need to be
  // greater than or equal to 0.2.0.
  Option<string> dvdcli = os::which("dvdcli");
  if (dvdcli.isNone()) {
    return Error("The 'docker/volume' isolator cannot get dvdcli command");
  }

  VLOG(1) << "Found 'dvdcli' at '" << dvdcli.get() << "'";

  Try<Owned<DriverClient>> client = DriverClient::create(dvdcli.get());
  if (client.isError()) {
    return Error(
        "Unable to create docker volume driver client: " + client.error());
  }

  // Create the docker volume information root directory if it does
  // not exist, this directory is used to checkpoint the docker
  // volumes used by containers.
  Try<Nothing> mkdir = os::mkdir(paths::ROOT_DIR);
  if (mkdir.isError()) {
    return Error(
        "Failed to create docker volume information root directory at '" +
        string(paths::ROOT_DIR) + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(paths::ROOT_DIR);
  if (!rootDir.isSome()) {
    return Error(
        "Failed to determine canonical path of docker volume information root "
        "directory at '" + string(paths::ROOT_DIR) + "': " +
        (rootDir.isError() ? rootDir.error() : "No such file or directory"));
  }

  VLOG(1) << "Initialized the docker volume information root directory at '"
          << rootDir.get() << "'";

  Owned<MesosIsolatorProcess> process(
      new DockerVolumeIsolatorProcess(
          flags,
          rootDir.get(),
          client.get()));

  return new MesosIsolator(process);
}


Future<Nothing> DockerVolumeIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<Option<ContainerLaunchInfo>> DockerVolumeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return None();
}


Future<Nothing> DockerVolumeIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> DockerVolumeIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> DockerVolumeIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> DockerVolumeIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return ResourceStatistics();
}


Future<Nothing> DockerVolumeIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
