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

#include <process/collect.hpp>

#include <stout/os.hpp>

#include "slave/flags.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/mesos/isolators/docker/volume/isolator.hpp"

namespace paths = mesos::internal::slave::docker::volume::paths;

using std::list;
using std::string;
using std::vector;

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
  const ExecutorInfo& executorInfo = containerConfig.executor_info();

  if (!executorInfo.has_container()) {
    return None();
  }

  if (executorInfo.container().type() != ContainerInfo::MESOS) {
    return Failure(
        "Can only prepare docker volume driver for a MESOS container");
  }

  // The hashset is used to check if there are duplicated docker
  // volume for the same container.
  hashset<DockerVolume> volumes;

  // Represents mounts that will be sent to the driver client.
  struct Mount
  {
    DockerVolume volume;
    hashmap<string, string> options;
  };

  vector<Mount> mounts;

  // The mount points in the container.
  vector<string> targets;

  foreach (const Volume& _volume, executorInfo.container().volumes()) {
    if (!_volume.has_source()) {
      continue;
    }

    if (_volume.source().type() != Volume::Source::DOCKER_VOLUME) {
      VLOG(1) << "Ignored volume type '" << _volume.source().type()
              << "' for container " << containerId << " as only "
              << "'DOCKER_VOLUME' was supported by the docker "
              << "volume isolator";
      continue;
    }

    const string driver = _volume.source().docker_volume().driver();
    const string name = _volume.source().docker_volume().name();

    DockerVolume volume;
    volume.set_driver(driver);
    volume.set_name(name);

    if (volumes.contains(volume)) {
      return Failure(
          "Found duplicate docker volume with driver '" +
          driver + "' and name '" + name + "'");
    }

    // Determine driver options.
    hashmap<string, string> options;
    if (_volume.source().docker_volume().has_driver_options()) {
      foreach (const Parameter& parameter,
               _volume.source().docker_volume().driver_options().parameter()) {
        options[parameter.key()] = parameter.value();
      }
    }

    // Determine the mount point.
    string target;

    if (containerConfig.has_rootfs()) {
      target = path::join(
          containerConfig.rootfs(),
          _volume.container_path());
    } else {
      target = path::join(
          containerConfig.directory(),
          _volume.container_path());
    }

    Mount mount;
    mount.volume = volume;
    mount.options = options;

    volumes.insert(volume);
    mounts.push_back(mount);
    targets.push_back(target);
  }

  // Create the container directory.
  const string containerDir =
    paths::getContainerDir(rootDir, containerId.value());

  Try<Nothing> mkdir = os::mkdir(containerDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create the container directory at '" +
        containerDir + "': " + mkdir.error());
  }

  // Create DockerVolumes protobuf message to checkpoint.
  DockerVolumes state;
  foreach (const DockerVolume& volume, volumes) {
    state.add_volumes()->CopyFrom(volume);
  }

  const string volumesPath =
    paths::getVolumesPath(rootDir, containerId.value());

  Try<Nothing> checkpoint = state::checkpoint(
      volumesPath,
      stringify(JSON::protobuf(state)));

  if (checkpoint.isError()) {
    return Failure(
        "Failed to checkpoint docker volumes at '" +
        volumesPath + "': " + checkpoint.error());
  }

  VLOG(1) << "Successfully created checkpoint at '" << volumesPath << "'";

  infos.put(containerId, Owned<Info>(new Info(volumes)));

  // Invoke driver client to create the mount.
  list<Future<string>> futures;
  foreach (const Mount& mount, mounts) {
    futures.push_back(client->mount(
        mount.volume.driver(),
        mount.volume.name(),
        mount.options));
  }

  // NOTE: Wait for all `mount()` to finish before returning to make
  // sure `unmount()` is not called (via 'cleanup()') if some mount on
  // is still pending.
  return await(futures)
    .then(defer(
        PID<DockerVolumeIsolatorProcess>(this),
        &DockerVolumeIsolatorProcess::_prepare,
        containerId,
        targets,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> DockerVolumeIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const vector<string>& targets,
    const list<Future<string>>& futures)
{
  ContainerLaunchInfo launchInfo;
  launchInfo.set_namespaces(CLONE_NEWNS);

  vector<string> messages;
  vector<string> sources;
  foreach (const Future<string>& future, futures) {
    if (!future.isReady()) {
      messages.push_back(future.isFailed() ? future.failure() : "discarded");
      continue;
    }

    sources.push_back(strings::trim(future.get()));
  }

  if (!messages.empty()) {
    return Failure(strings::join("\n", messages));
  }

  CHECK_EQ(sources.size(), targets.size());

  for (size_t i = 0; i < sources.size(); i++) {
    const string source = sources[i];
    const string target = targets[i];

    VLOG(1) << "Mounting docker volume mount point '" << source
            << "' to '" << target  << "' for container '"
            << containerId << "'";

    // Create the mount point if it does not exist.
    Try<Nothing> mkdir = os::mkdir(target);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create mount point at '" +
          target + "': " + mkdir.error());
    }

    const string command = "mount -n --rbind " + source + " " + target;

    launchInfo.add_commands()->set_value(command);
  }

  return launchInfo;
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
