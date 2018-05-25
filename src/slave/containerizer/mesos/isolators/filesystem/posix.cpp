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

#include <process/id.hpp>

#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/posix.hpp"

using namespace process;

using std::string;
using std::vector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

PosixFilesystemIsolatorProcess::PosixFilesystemIsolatorProcess(
    const Flags& _flags)
  : ProcessBase(process::ID::generate("posix-filesystem-isolator")),
    flags(_flags) {}


PosixFilesystemIsolatorProcess::~PosixFilesystemIsolatorProcess() {}


Try<Isolator*> PosixFilesystemIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new PosixFilesystemIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Nothing> PosixFilesystemIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    infos.put(state.container_id(), Owned<Info>(new Info(state.directory())));
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> PosixFilesystemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  const ExecutorInfo& executorInfo = containerConfig.executor_info();

  if (executorInfo.has_container()) {
    CHECK_EQ(executorInfo.container().type(), ContainerInfo::MESOS);

    // Return failure if the container change the filesystem root
    // because the symlinks will become invalid in the new root.
    if (executorInfo.container().mesos().has_image()) {
      return Failure("Container root filesystems not supported");
    }

    if (executorInfo.container().volumes().size() > 0) {
      return Failure("Volumes in ContainerInfo is not supported");
    }
  }

  infos.put(containerId, Owned<Info>(new Info(containerConfig.directory())));

  return update(containerId, executorInfo.resources())
      .then([]() -> Future<Option<ContainerLaunchInfo>> { return None(); });
}


Future<Nothing> PosixFilesystemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  // TODO(jieyu): Currently, we only allow non-nested relative
  // container paths for volumes. This is enforced by the master. For
  // those volumes, we create symlinks in the executor directory.
  Resources current = info->resources;

  // We first remove unneeded persistent volumes.
  foreach (const Resource& resource, current.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating symlink for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (resources.contains(resource)) {
      continue;
    }

    string link = path::join(info->directory, containerPath);

    LOG(INFO) << "Removing symlink '" << link << "' for persistent volume "
              << resource << " of container " << containerId;

    Try<Nothing> rm = os::rm(link);
    if (rm.isError()) {
      return Failure(
          "Failed to remove the symlink for the unneeded "
          "persistent volume at '" + link + "'");
    }
  }

  // Get user and group info for this task based on the task's sandbox.
  struct stat s;
  if (::stat(info->directory.c_str(), &s) < 0) {
    return Failure("Failed to get ownership for '" + info->directory +
                   "': " + os::strerror(errno));
  }

  const uid_t uid = s.st_uid;
  const gid_t gid = s.st_gid;

  // We then link additional persistent volumes.
  foreach (const Resource& resource, resources.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating symlink for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (current.contains(resource)) {
      continue;
    }

    string original = paths::getPersistentVolumePath(flags.work_dir, resource);

    bool isVolumeInUse = false;

    foreachpair (const ContainerID& _containerId,
                 const Owned<Info>& info,
                 infos) {
      // Skip self.
      if (_containerId == containerId) {
        continue;
      }

      if (info->resources.contains(resource)) {
        isVolumeInUse = true;
        break;
      }
    }

    // Set the ownership of the persistent volume to match that of the sandbox
    // directory if the volume is not already in use. If the volume is
    // currently in use by other containers, tasks in this container may fail
    // to read from or write to the persistent volume due to incompatible
    // ownership and file system permissions.
    if (!isVolumeInUse) {
      // TODO(hausdorff): (MESOS-5461) Persistent volumes maintain the invariant
      // that they are used by one task at a time. This is currently enforced by
      // `os::chown`. Windows does not support `os::chown`, we will need to
      // revisit this later.
#ifndef __WINDOWS__
      LOG(INFO) << "Changing the ownership of the persistent volume at '"
                << original << "' with uid " << uid << " and gid " << gid;

      Try<Nothing> chown = os::chown(uid, gid, original, false);

      if (chown.isError()) {
        return Failure(
            "Failed to change the ownership of the persistent volume at '" +
            original + "' with uid " + stringify(uid) +
            " and gid " + stringify(gid) + ": " + chown.error());
      }
#endif
    }

    string link = path::join(info->directory, containerPath);

    if (os::exists(link)) {
      // NOTE: This is possible because 'info->resources' will be
      // reset when slave restarts and recovers. When the slave calls
      // 'containerizer->update' after the executor reregisters,
      // we'll try to relink all the already symlinked volumes.
      Result<string> realpath = os::realpath(link);
      if (!realpath.isSome()) {
        return Failure(
            "Failed to get the realpath of symlink '" + link + "': " +
            (realpath.isError() ? realpath.error() : "No such directory"));
      }

      // A sanity check to make sure the target of the symlink does
      // not change. In fact, this is not supposed to happen.
      // NOTE: Here, we compare the realpaths because 'original' might
      // contain symbolic links.
      Result<string> _original = os::realpath(original);
      if (!_original.isSome()) {
        return Failure(
            "Failed to get the realpath of volume '" + original + "': " +
            (_original.isError() ? _original.error() : "No such directory"));
      }

      if (realpath.get() != _original.get()) {
        return Failure(
            "The existing symlink '" + link + "' points to '" +
            _original.get() + "' and the new target is '" +
            realpath.get() + "'");
      }
    } else {
      LOG(INFO) << "Adding symlink from '" << original << "' to '"
                << link << "' for persistent volume " << resource
                << " of container " << containerId;

      // NOTE: We cannot enforce read-only access given the symlink without
      // changing the source so we just log a warning here.
      if (resource.disk().volume().mode() == Volume::RO) {
        LOG(WARNING) << "Allowing read-write access to read-only volume '"
                     << original << "' of container " << containerId;
      }

      Try<Nothing> symlink = ::fs::symlink(original, link);
      if (symlink.isError()) {
        return Failure(
            "Failed to symlink persistent volume from '" +
            original + "' to '" + link + "'");
      }
    }
  }

  // Store the updated resources.
  info->resources = resources;

  return Nothing();
}


Future<Nothing> PosixFilesystemIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Symlinks for persistent resources will be removed when the work
  // directory is GC'ed, therefore no need to do explicit cleanup.
  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
