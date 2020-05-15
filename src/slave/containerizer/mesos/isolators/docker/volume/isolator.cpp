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

#include <sys/mount.h>

#include <process/collect.hpp>
#include <process/id.hpp>

#include <stout/os.hpp>

#include <stout/os/realpath.hpp>
#include <stout/os/which.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/ns.hpp"

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
using mesos::slave::ContainerMountInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerVolumeIsolatorProcess::DockerVolumeIsolatorProcess(
    const Flags& _flags,
    const string& _rootDir,
    const Owned<DriverClient>& _client)
  : ProcessBase(process::ID::generate("docker-volume-isolator")),
    flags(_flags),
    rootDir(_rootDir),
    client(_client) {}


DockerVolumeIsolatorProcess::~DockerVolumeIsolatorProcess() {}


bool DockerVolumeIsolatorProcess::supportsNesting()
{
  return true;
}


bool DockerVolumeIsolatorProcess::supportsStandalone()
{
  return true;
}


Try<Isolator*> DockerVolumeIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The 'docker/volume' isolator requires root permissions");
  }

  Try<bool> supported = ns::supported(CLONE_NEWNS);
  if (supported.isError() || !supported.get()) {
    return Error(
        "The 'docker/volume' isolator requires mount namespace support");
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

  Try<Isolator*> isolator =
    DockerVolumeIsolatorProcess::_create(flags, client.get());

  if (isolator.isError()) {
    return Error(isolator.error());
  }

  return isolator.get();
}


Try<Isolator*> DockerVolumeIsolatorProcess::_create(
    const Flags& flags,
    const Owned<DriverClient>& client)
{
  // Create the docker volume information root directory if it does
  // not exist, this directory is used to checkpoint the docker
  // volumes used by containers.
  Try<Nothing> mkdir = os::mkdir(flags.docker_volume_checkpoint_dir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create docker volume information root directory at '" +
        flags.docker_volume_checkpoint_dir + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(flags.docker_volume_checkpoint_dir);
  if (!rootDir.isSome()) {
    return Error(
        "Failed to determine canonical path of docker volume information root "
        "directory at '" + flags.docker_volume_checkpoint_dir + "': " +
        (rootDir.isError() ? rootDir.error() : "No such file or directory"));
  }

  VLOG(1) << "Initialized the docker volume information root directory at '"
          << rootDir.get() << "'";

  Owned<MesosIsolatorProcess> process(
      new DockerVolumeIsolatorProcess(
          flags,
          rootDir.get(),
          client));

  return new MesosIsolator(process);
}


Future<Nothing> DockerVolumeIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  if (!os::exists(rootDir)) {
    VLOG(1) << "The checkpoint directory at '" << rootDir
            << "' does not exist. Skipping recovery.";

    return Nothing();
  }

  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();

    Try<Nothing> recover = _recover(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover docker volumes for container " +
          stringify(containerId) + ": " + recover.error());
    }
  }

  // Recover any orphan containers that we might have check pointed.
  // These orphan containers will be destroyed by the containerizer
  // through the regular cleanup path. See MESOS-2367 for details.
  foreach (const ContainerID& containerId, orphans) {
    Try<Nothing> recover = _recover(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover docker volumes for orphan container " +
          stringify(containerId) + ": " + recover.error());
    }
  }

  // Walk through all the checkpointed containers to determine if
  // there are any 'unknown orphan' containers.
  Try<list<string>> entries = os::ls(rootDir);
  if (entries.isError()) {
    return Failure(
        "Unable to list docker volume checkpoint directory '" +
        rootDir + "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    ContainerID containerId;
    containerId.set_value(Path(entry).basename());

    bool recovered = false;
    // Check if this container has already been recovered.
    //
    // NOTE: We cannot use `infos.contains()` to check the recovery
    // status of this container, since the recovered `ContainerID` has
    // only the `value` set. We don't checkpoint the `parent`
    // associated with the container. Therefore, since we don't know
    // if the container is a nested container or not, we have to
    // traverse each entry of the `infos` hashmap and compare the
    // value fields to identify if the container has already been
    // recovered.
    foreachkey (const ContainerID& _containerId, infos) {
      if (_containerId.value() == containerId.value()) {
        recovered = true;
        break;
      }
    }

    if (recovered) {
      continue;
    }

    // An unknown orphan container. Recover it and then clean it up.
    Try<Nothing> recover = _recover(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover docker volumes for orphan container " +
          stringify(containerId) + ": " + recover.error());
    }

    LOG(INFO) << "Cleanup volumes for unknown orphaned "
              << "container " << containerId;

    cleanup(containerId);
  }

  return Nothing();
}


Try<Nothing> DockerVolumeIsolatorProcess::_recover(
    const ContainerID& containerId)
{
  // NOTE: This method will add an 'Info' to 'infos' only if the
  // container was launched by the docker volume isolator.

  const string containerDir =
    paths::getContainerDir(rootDir, containerId.value());

  if (!os::exists(containerDir)) {
    // This may occur in the following cases:
    //   1. Executor has exited and the isolator has removed the
    //      container directory in '_cleanup()' but agent dies before
    //      noticing this.
    //   2. Agent dies before the isolator creates the container
    //      directory in 'prepare()'.
    // For the above cases, we do not need to do anything since there
    // is nothing to clean up after agent restarts.
    return Nothing();
  }

  const string volumesPath =
    paths::getVolumesPath(rootDir, containerId.value());

  if (!os::exists(volumesPath)) {
    // This could happen if the slave died after creating the container
    // directory but before it checkpointed anything in it.
    LOG(WARNING) << "The docker volumes checkpointed at '" << volumesPath
                 << "' for container " << containerId << " does not exist";

    // Construct an info object with empty docker volumes since no docker
    // volumes are mounted yet for this container, and this container will
    // be cleaned up by containerizer (as known orphan container) or by
    // `recover` (as unknown orphan container).
    infos.put(containerId, Owned<Info>(new Info(hashset<DockerVolume>())));

    return Nothing();
  }

  Result<string> read = state::read<string>(volumesPath);
  if (read.isError()) {
    return Error(
        "Failed to read docker volumes checkpoint file '" +
        volumesPath + "': " + read.error());
  }

  if (read->empty()) {
    // This could happen if the slave is hard rebooted after the file is
    // created but before the data is synced on disk.
    LOG(WARNING) << "The docker volumes checkpointed at '" << volumesPath
                 << "' for container " << containerId << " is empty";

    // Construct an info object with empty docker volumes since no docker
    // volumes are mounted yet for this container, and this container will
    // be cleaned up by containerizer (as known orphan container) or by
    // `recover` (as unknown orphan container).
    infos.put(containerId, Owned<Info>(new Info(hashset<DockerVolume>())));

    return Nothing();
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<DockerVolumes> parse = ::protobuf::parse<DockerVolumes>(json.get());
  if (parse.isError()) {
    return Error("Protobuf parse failed: " + parse.error());
  }

  hashset<DockerVolume> volumes;

  foreach (const DockerVolume& volume, parse->volumes()) {
    VLOG(1) << "Recovering docker volume with driver '"
            << volume.driver() << "' and name '" << volume.name()
            << "' for container " << containerId;

    if (volumes.contains(volume)) {
      return Error(
          "Duplicate docker volume with driver '" + volume.driver() + "' "
          "and name '" + volume.name() + "'");
    }

    volumes.insert(volume);
  }

  infos.put(containerId, Owned<Info>(new Info(volumes)));

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> DockerVolumeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  if (containerConfig.container_info().type() != ContainerInfo::MESOS) {
    return Failure(
        "Can only prepare docker volume driver for a MESOS container");
  }

  // The hashset is used to check if there are duplicated docker
  // volume for the same container.
  hashset<DockerVolume> volumeSet;

  // Represents mounts that will be sent to the driver client.
  struct Mount
  {
    DockerVolume volume;
    hashmap<string, string> options;
  };


  // TODO(qianzhang): Here we use vector to ensure the order of mount target,
  // mount source and volume mode which is kind of hacky, we could consider
  // to introduce a dedicated struct for it in future.
  vector<Mount> mounts;

  // The mount points in the container.
  vector<string> targets;

  vector<Volume::Mode> volumeModes;

  foreach (const Volume& _volume, containerConfig.container_info().volumes()) {
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

    const string& name = _volume.source().docker_volume().name();

    if (!_volume.source().docker_volume().has_driver()) {
      return Failure("The volume driver is not specified for volume '" +
                     name + "' with container " + stringify(containerId));
    }

    const string& driver = _volume.source().docker_volume().driver();

    DockerVolume volume;
    volume.set_driver(driver);
    volume.set_name(name);

    if (volumeSet.contains(volume)) {
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

    // Determine the target of the mount.
    string target;

    // The logic to determine a volume mount target is identical to
    // linux filesystem isolator, because docker volume isolator has
    // a dependency on that isolator, and it assumes that if the
    // container specifies a rootfs the sandbox is already bind
    // mounted into the container.
    if (path::is_absolute(_volume.container_path())) {
      // To specify a docker volume for a container, operators should
      // be allowed to define the 'container_path' either as an absolute
      // path or a relative path. Please see linux filesystem isolator
      // for detail.
      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            _volume.container_path());

        Try<Nothing> mkdir = os::mkdir(target);
        if (mkdir.isError()) {
          return Failure(
              "Failed to create the target of the mount at '" +
              target + "': " + mkdir.error());
        }
      } else {
        target = _volume.container_path();

        if (!os::exists(target)) {
          return Failure("Absolute container path '" + target + "' "
                         "does not exist");
        }
      }
    } else {
      if (containerConfig.has_rootfs()) {
        target = path::join(containerConfig.rootfs(),
                            flags.sandbox_directory,
                            _volume.container_path());
      } else {
        target = path::join(containerConfig.directory(),
                            _volume.container_path());
      }

      // NOTE: We cannot create the mount point at 'target' if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside 'target'. So we should always create
      // the mount point in 'directory'.
      string mountPoint = path::join(
          containerConfig.directory(),
          _volume.container_path());

      Try<Nothing> mkdir = os::mkdir(mountPoint);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the target of the mount at '" +
            mountPoint + "': " + mkdir.error());
      }
    }

    Mount mount;
    mount.volume = volume;
    mount.options = options;

    volumeSet.insert(volume);
    mounts.push_back(mount);
    targets.push_back(target);
    volumeModes.push_back(_volume.mode());
  }

  // It is possible that there is no external volume specified for
  // this container. We avoid checkpointing empty state and creating
  // an empty `Info`.
  if (volumeSet.empty()) {
    return None();
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
  foreach (const DockerVolume& volume, volumeSet) {
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

  infos.put(containerId, Owned<Info>(new Info(volumeSet)));

  // Invoke driver client to create the mount.
  vector<Future<string>> futures;
  futures.reserve(mounts.size());
  foreach (const Mount& mount, mounts) {
    futures.push_back(this->mount(
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
        volumeModes,
        containerConfig.has_user()
          ? containerConfig.user()
          : Option<string>::none(),
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> DockerVolumeIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const vector<string>& targets,
    const vector<Volume::Mode>& volumeModes,
    const Option<string>& user,
    const vector<Future<string>>& futures)
{
  ContainerLaunchInfo launchInfo;
  launchInfo.add_clone_namespaces(CLONE_NEWNS);

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
  CHECK_EQ(sources.size(), volumeModes.size());

  for (size_t i = 0; i < sources.size(); i++) {
    const string& source = sources[i];
    const string& target = targets[i];
    const Volume::Mode volumeMode = volumeModes[i];

    if (flags.docker_volume_chown && user.isSome() && user.get() != "root") {
      LOG(INFO) << "Changing the ownership of the docker volume at '"
                << source << "' to user '" << user.get() << "' for container "
                << containerId;

      Try<Nothing> chown = os::chown(user.get(), source, false);
      if (chown.isError()) {
        return Failure(
            "Failed to set '" + user.get() + "' as the docker volume '" +
            source + "' owner: " + chown.error());
      }
    }

    LOG(INFO) << "Mounting docker volume mount point '" << source
              << "' to '" << target << "' for container " << containerId;

    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        source,
        target,
        MS_BIND | MS_REC | (volumeMode == Volume::RO ? MS_RDONLY : 0));
  }

  return launchInfo;
}


Future<Nothing> DockerVolumeIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;

    return Nothing();
  }

  // Make sure the container we are cleaning up doesn't have any
  // children (they should have already been cleaned up by a previous
  // call if it had any).
  foreachkey (const ContainerID& containerId_, infos) {
    if (containerId_.has_parent() && containerId_.parent() == containerId) {
      return Failure(
          "Failed to clean up container " + stringify(containerId) +
          ": it has child container " + stringify(containerId_) +
          " which is not cleaned up yet");
    }
  }

  hashmap<DockerVolume, int> references;
  foreachvalue (const Owned<Info>& info, infos) {
    foreach (const DockerVolume& volume, info->volumes) {
      if (!references.contains(volume)) {
        references[volume] = 1;
      } else {
        references[volume]++;
      }
    }
  }

  vector<Future<Nothing>> futures;

  foreach (const DockerVolume& volume, infos[containerId]->volumes) {
    if (references.contains(volume) && references[volume] > 1) {
      VLOG(1) << "Cannot unmount the volume with driver '"
              << volume.driver() << "' and name '" << volume.name()
              << "' for container " << containerId
              << " since its reference count is " << references[volume];
      continue;
    }

    LOG(INFO) << "Unmounting the volume with driver '"
              << volume.driver() << "' and name '" << volume.name()
              << "' for container " << containerId;

    // Invoke dvdcli client to unmount the docker volume.
    futures.push_back(this->unmount(volume.driver(), volume.name()));
  }

  // Erase the `Info` struct of this container before unmounting the volumes.
  // This is to ensure the reference count of the volume will not be wrongly
  // increased if unmounting volumes fail, otherwise next time when another
  // container using the same volume is destroyed, we would NOT unmount the
  // volume since its reference count would be larger than 1.
  infos.erase(containerId);

  return await(futures)
    .then(defer(
        PID<DockerVolumeIsolatorProcess>(this),
        &DockerVolumeIsolatorProcess::_cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> DockerVolumeIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const vector<Future<Nothing>>& futures)
{
  vector<string> messages;
  foreach (const Future<Nothing>& future, futures) {
    if (!future.isReady()) {
      messages.push_back(future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!messages.empty()) {
    return Failure(strings::join("\n", messages));
  }

  const string containerDir =
    paths::getContainerDir(rootDir, containerId.value());

  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    return Failure(
        "Failed to remove the checkpoint directory at '" +
        containerDir + "': " + rmdir.error());
  }

  LOG(INFO) << "Removed the checkpoint directory at '" << containerDir
            << "' for container " << containerId;

  return Nothing();
}


Future<string> DockerVolumeIsolatorProcess::mount(
    const string& driver,
    const string& name,
    const hashmap<string, string>& options)
{
  DockerVolume volume;
  volume.set_driver(driver);
  volume.set_name(name);

  return sequences[volume].add<string>(
      defer(PID<DockerVolumeIsolatorProcess>(this), [=]() -> Future<string> {
        return _mount(driver, name, options);
      }));
}


Future<string> DockerVolumeIsolatorProcess::_mount(
    const string& driver,
    const string& name,
    const hashmap<string, string>& options)
{
  return client->mount(driver, name, options);
}


Future<Nothing> DockerVolumeIsolatorProcess::unmount(
    const string& driver,
    const string& name)
{
  DockerVolume volume;
  volume.set_driver(driver);
  volume.set_name(name);

  return sequences[volume].add<Nothing>(
      defer(PID<DockerVolumeIsolatorProcess>(this), [=]() -> Future<Nothing> {
        return _unmount(driver, name);
      }));
}


Future<Nothing> DockerVolumeIsolatorProcess::_unmount(
    const string& driver,
    const string& name)
{
  return client->unmount(driver, name);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
