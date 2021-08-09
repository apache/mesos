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
#include <string>
#include <vector>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/os.hpp>

#include <stout/os/realpath.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/state.hpp"

#include "slave/containerizer/mesos/isolators/volume/csi/isolator.hpp"
#include "slave/containerizer/mesos/isolators/volume/csi/paths.hpp"

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

using AccessMode = Volume::Source::CSIVolume::VolumeCapability::AccessMode;


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

  const string csiRootDir = path::join(flags.work_dir, csi::paths::CSI_DIR);

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
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();

    Try<Nothing> recover = recoverContainer(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover CSI volumes for container " +
          stringify(containerId) + ": " + recover.error());
    }
  }

  // Recover any orphan containers that we might have check pointed.
  // These orphan containers will be destroyed by the containerizer
  // through the regular cleanup path. See MESOS-2367 for details.
  foreach (const ContainerID& containerId, orphans) {
    Try<Nothing> recover = recoverContainer(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover CSI volumes for orphan container " +
          stringify(containerId) + ": " + recover.error());
    }
  }

  // Walk through all the checkpointed containers to determine if
  // there are any unknown orphan containers.
  Try<list<string>> entries = os::ls(rootDir);
  if (entries.isError()) {
    return Failure(
        "Unable to list CSI volume checkpoint directory '" +
        rootDir + "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    ContainerID containerId =
      protobuf::parseContainerId(Path(entry).basename());

    // Check if this container has already been recovered.
    if (infos.contains(containerId)) {
      continue;
    }

    // An unknown orphan container. Recover it and then clean it up.
    Try<Nothing> recover = recoverContainer(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover CSI volumes for orphan container " +
          stringify(containerId) + ": " + recover.error());
    }

    LOG(INFO) << "Cleaning up CSI volumes for unknown orphaned "
              << "container " << containerId;

    cleanup(containerId);
  }

  return Nothing();
}


Try<Nothing> VolumeCSIIsolatorProcess::recoverContainer(
    const ContainerID& containerId)
{
  const string containerDir = csi::paths::getContainerDir(rootDir, containerId);
  if (!os::exists(containerDir)) {
    // This may occur in the following cases:
    //   1. The container has exited and the isolator has removed the
    //      container directory in '_cleanup()' but agent dies before
    //      noticing this.
    //   2. Agent dies before the isolator checkpoints CSI volumes for
    //      the container in 'prepare()'.
    // For the above cases, we do not need to do anything since there
    // is nothing to clean up for this container after agent restarts.
    return Nothing();
  }

  const string volumesPath = csi::paths::getVolumesPath(rootDir, containerId);
  if (!os::exists(volumesPath)) {
    // This may occur if agent dies after creating the container directory
    // but before it checkpoints anything in it.
    LOG(WARNING) << "The CSI volumes checkpoint file expected at '"
                 << volumesPath << "' for container " << containerId
                 << " does not exist";

    // Construct an info object with empty CSI volumes since no CSI volumes
    // are mounted yet for this container, and this container will be cleaned
    // up by containerizer (as known orphan container) or by `recover` (as
    // unknown orphan container).
    infos.put(containerId, Owned<Info>(new Info(hashset<CSIVolume>())));

    return Nothing();
  }

  Result<CSIVolumes> read = state::read<CSIVolumes>(volumesPath);
  if (read.isError()) {
    return Error(
        "Failed to read the CSI volumes checkpoint file '" +
        volumesPath + "': " + read.error());
  } else if (read.isNone()) {
    // This could happen if agent is hard rebooted after the checkpoint file is
    // created but before the data is synced on disk.
    LOG(WARNING) << "The CSI volumes checkpointed at '" << volumesPath
                 << "' for container " << containerId << " is empty";

    // Construct an info object with empty CSI volumes since no CSI volumes
    // are mounted yet for this container, and this container will be cleaned
    // up by containerizer (as known orphan container) or by `recover` (as
    // unknown orphan container).
    infos.put(containerId, Owned<Info>(new Info(hashset<CSIVolume>())));

    return Nothing();
  }

  hashset<CSIVolume> volumes;
  foreach (const CSIVolume& volume, read->volumes()) {
    VLOG(1) << "Recovering CSI volume with plugin '" << volume.plugin_name()
            << "' and ID '" << volume.id() << "' for container " << containerId;

    if (volumes.contains(volume)) {
      return Error(
          "Duplicate CSI volume with plugin '" + volume.plugin_name() +
          "' and ID '" + volume.id() + "'");
    }

    volumes.insert(volume);
  }

  infos.put(containerId, Owned<Info>(new Info(volumes)));

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> VolumeCSIIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  if (containerConfig.container_info().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare CSI volumes for a MESOS container");
  }

  // The hashset is used to check if there are duplicated CSI volumes for the
  // same container.
  hashset<CSIVolume> volumeSet;

  // Represents the CSI volume mounts that we want to do for the container.
  vector<Mount> mounts;

  foreach (const Volume& _volume, containerConfig.container_info().volumes()) {
    if (!_volume.has_source() ||
        !_volume.source().has_type() ||
        _volume.source().type() != Volume::Source::CSI_VOLUME) {
      continue;
    }

    CHECK(_volume.source().has_csi_volume());
    CHECK(_volume.source().csi_volume().has_static_provisioning());

    const Volume::Source::CSIVolume& csiVolume = _volume.source().csi_volume();
    const string& pluginName = csiVolume.plugin_name();
    const string& volumeId = csiVolume.static_provisioning().volume_id();
    const AccessMode& accessMode =
      csiVolume.static_provisioning().volume_capability().access_mode();

    if ((accessMode.mode() == AccessMode::SINGLE_NODE_READER_ONLY ||
         accessMode.mode() == AccessMode::MULTI_NODE_READER_ONLY) &&
        _volume.mode() == Volume::RW) {
      return Failure(
          "Cannot use the read-only volume '" +
          volumeId + "' in read-write mode");
    }

    CSIVolume volume;
    volume.set_plugin_name(pluginName);
    volume.set_id(volumeId);

    if (volumeSet.contains(volume)) {
      return Failure(
          "Found duplicate CSI volume with plugin '" +
          pluginName + "' and volume ID '" + volumeId + "'");
    }

    // Determine the target of the mount.
    string target;

    // The logic to determine a volume mount target is identical to Linux
    // filesystem isolator, because this isolator has a dependency on that
    // isolator, and it assumes that if the container specifies a rootfs
    // the sandbox is already bind mounted into the container.
    if (path::is_absolute(_volume.container_path())) {
      // To specify a CSI volume for a container, frameworks should be allowed
      // to define the `container_path` either as an absolute path or a relative
      // path. Please see Linux filesystem isolator for details.
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

      // NOTE: We cannot create the mount point at `target` if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside `target`. So we should always create
      // the mount point in `directory`.
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
    mount.volume = _volume;
    mount.csiVolume = volume;
    mount.target = target;

    mounts.push_back(mount);
    volumeSet.insert(volume);
  }

  if (volumeSet.empty()) {
    return None();
  }

  // Create the `CSIVolumes` protobuf message to checkpoint.
  CSIVolumes state;
  foreach (const CSIVolume& volume, volumeSet) {
    state.add_volumes()->CopyFrom(volume);
  }

  const string volumesPath = csi::paths::getVolumesPath(rootDir, containerId);
  Try<Nothing> checkpoint = state::checkpoint(volumesPath, state);
  if (checkpoint.isError()) {
    return Failure(
        "Failed to checkpoint CSI volumes at '" +
        volumesPath + "': " + checkpoint.error());
  }

  VLOG(1) << "Successfully created checkpoint at '" << volumesPath << "'";

  infos.put(containerId, Owned<Info>(new Info(volumeSet)));

  // Invoke CSI server to publish the volumes.
  vector<Future<string>> futures;
  futures.reserve(mounts.size());
  foreach (const Mount& mount, mounts) {
    futures.push_back(csiServer->publishVolume(mount.volume));
  }

  return await(futures)
    .then(defer(
        PID<VolumeCSIIsolatorProcess>(this),
        &VolumeCSIIsolatorProcess::_prepare,
        containerId,
        mounts,
        containerConfig.has_user()
          ? containerConfig.user()
          : Option<string>::none(),
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> VolumeCSIIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const vector<Mount>& mounts,
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

  CHECK_EQ(sources.size(), mounts.size());

  for (size_t i = 0; i < sources.size(); i++) {
    const string& source = sources[i];
    const Mount& mount = mounts[i];

    if (user.isSome() && user.get() != "root") {
      bool isVolumeInUse = false;

      // Check if the volume is currently used by another container.
      foreachpair (const ContainerID& _containerId,
                   const Owned<Info>& info,
                   infos) {
        // Skip self.
        if (_containerId == containerId) {
          continue;
        }

        if (info->volumes.contains(mount.csiVolume)) {
          isVolumeInUse = true;
          break;
        }
      }

      if (!isVolumeInUse) {
        LOG(INFO) << "Changing the ownership of the CSI volume at '" << source
                  << "' to user '" << user.get() << "' for container "
                  << containerId;

        Try<Nothing> chown = os::chown(user.get(), source, false);
        if (chown.isError()) {
          return Failure(
              "Failed to set '" + user.get() + "' as the owner of the "
              "CSI volume at '" + source + "': " + chown.error());
        }
      } else {
        LOG(INFO) << "Leaving the ownership of the CSI volume at '"
                  << source << "' unchanged because it is in use";
      }
    }

    LOG(INFO) << "Mounting CSI volume mount point '" << source
              << "' to '" << mount.target << "' for container " << containerId;

    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        source,
        mount.target,
        MS_BIND | MS_REC | (mount.volume.mode() == Volume::RO ? MS_RDONLY : 0));
  }

  return launchInfo;
}


Future<Nothing> VolumeCSIIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container " << containerId;
    return Nothing();
  }

  hashmap<CSIVolume, int> references;
  foreachvalue (const Owned<Info>& info, infos) {
    foreach (const CSIVolume& volume, info->volumes) {
      if (!references.contains(volume)) {
        references[volume] = 1;
      } else {
        references[volume]++;
      }
    }
  }

  vector<Future<Nothing>> futures;

  foreach (const CSIVolume& volume, infos[containerId]->volumes) {
    if (references.contains(volume) && references[volume] > 1) {
      VLOG(1) << "Cannot unpublish the volume with plugin '"
              << volume.plugin_name() << "' and ID '" << volume.id()
              << "' for container " << containerId
              << " since its reference count is " << references[volume];
      continue;
    }

    LOG(INFO) << "Unpublishing the volume with plugin '"
              << volume.plugin_name() << "' and ID '" << volume.id()
              << "' for container " << containerId;

    // Invoke CSI server to unpublish the volumes.
    futures.push_back(
        csiServer->unpublishVolume(volume.plugin_name(), volume.id()));
  }

  // Erase the `Info` struct of this container before unpublishing the volumes.
  // This is to ensure the reference count of the volume will not be wrongly
  // increased if unpublishing volumes fail, otherwise next time when another
  // container using the same volume is destroyed, we would NOT unpublish the
  // volume since its reference count would be larger than 1.
  infos.erase(containerId);

  return await(futures)
    .then(defer(
        PID<VolumeCSIIsolatorProcess>(this),
        &VolumeCSIIsolatorProcess::_cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> VolumeCSIIsolatorProcess::_cleanup(
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

  const string containerDir = csi::paths::getContainerDir(rootDir, containerId);
  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    return Failure(
        "Failed to remove the container directory at '" +
        containerDir + "': " + rmdir.error());
  }

  LOG(INFO) << "Removed the container directory at '" << containerDir
            << "' for container " << containerId;

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
