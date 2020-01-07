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

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/touch.hpp>

#include "common/protobuf_utils.hpp"
#include "common/validation.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/isolators/volume/sandbox_path.hpp"

using std::string;
using std::vector;

using process::ErrnoFailure;
using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerMountInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> VolumeSandboxPathIsolatorProcess::create(
    const Flags& flags,
    VolumeGidManager* volumeGidManager)
{
  bool bindMountSupported = false;

  if (flags.launcher == "linux" &&
      strings::contains(flags.isolation, "filesystem/linux")) {
    bindMountSupported = true;
  }

  Owned<MesosIsolatorProcess> process(
      new VolumeSandboxPathIsolatorProcess(
          flags,
#ifdef __linux__
          volumeGidManager,
#endif // __linux__
          bindMountSupported));

  return new MesosIsolator(process);
}


VolumeSandboxPathIsolatorProcess::VolumeSandboxPathIsolatorProcess(
    const Flags& _flags,
#ifdef __linux__
    VolumeGidManager* _volumeGidManager,
#endif // __linux__
    bool _bindMountSupported)
  : ProcessBase(process::ID::generate("volume-sandbox-path-isolator")),
    flags(_flags),
#ifdef __linux__
    volumeGidManager(_volumeGidManager),
#endif // __linux__
    bindMountSupported(_bindMountSupported) {}


VolumeSandboxPathIsolatorProcess::~VolumeSandboxPathIsolatorProcess() {}


bool VolumeSandboxPathIsolatorProcess::supportsNesting()
{
  return true;
}


bool VolumeSandboxPathIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Nothing> VolumeSandboxPathIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    sandboxes[state.container_id()] = state.directory();
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> VolumeSandboxPathIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // Remember the sandbox location for each container (including
  // nested). This information is important for looking up sandbox
  // locations for parent containers.
  sandboxes[containerId] = containerConfig.directory();

  if (!containerConfig.has_container_info()) {
    return None();
  }

  const ContainerInfo& containerInfo = containerConfig.container_info();

  if (containerInfo.type() != ContainerInfo::MESOS) {
    return Failure("Only support MESOS containers");
  }

  if (!bindMountSupported && containerConfig.has_rootfs()) {
    return Failure(
        "The 'linux' launcher and 'filesystem/linux' isolator must be "
        "enabled to change the rootfs and bind mount");
  }

  ContainerLaunchInfo launchInfo;
  vector<Future<gid_t>> futures;

  foreach (const Volume& volume, containerInfo.volumes()) {
    // NOTE: The validation here is for backwards compatibility. For
    // example, if an old master (no validation code) is used to
    // launch a task with a volume.
    Option<Error> error = common::validation::validateVolume(volume);
    if (error.isSome()) {
      return Failure("Invalid volume: " + error->message);
    }

    Option<Volume::Source::SandboxPath> sandboxPath;

    // NOTE: This is the legacy way of specifying the Volume. The
    // 'host_path' can be relative in legacy mode, representing
    // SANDBOX_PATH volumes.
    if (volume.has_host_path() &&
        !path::is_absolute(volume.host_path())) {
      sandboxPath = Volume::Source::SandboxPath();
      sandboxPath->set_type(Volume::Source::SandboxPath::SELF);
      sandboxPath->set_path(volume.host_path());
    }

    if (volume.has_source() &&
        volume.source().has_type() &&
        volume.source().type() == Volume::Source::SANDBOX_PATH) {
      CHECK(volume.source().has_sandbox_path());

      if (path::is_absolute(volume.source().sandbox_path().path())) {
        return Failure(
            "Path '" + volume.source().sandbox_path().path() + "' "
            "in SANDBOX_PATH volume is absolute");
      }

      sandboxPath = volume.source().sandbox_path();
    }

    if (sandboxPath.isNone()) {
      continue;
    }

    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return Failure(
          "SANDBOX_PATH volume is not supported for DEBUG containers");
    }

    if (!bindMountSupported && path::is_absolute(volume.container_path())) {
      return Failure(
          "The 'linux' launcher and 'filesystem/linux' isolator "
          "must be enabled to support SANDBOX_PATH volume with "
          "absolute container path");
    }

    // TODO(jieyu): We need to check that source resolves under the
    // work directory because a user can potentially use a container
    // path like '../../abc'.

    if (!sandboxPath->has_type()) {
      return Failure("Unknown SANDBOX_PATH volume type");
    }

    // Prepare the source.
    string source;
    string sourceRoot; // The parent directory of 'source'.

    switch (sandboxPath->type()) {
      case Volume::Source::SandboxPath::SELF:
        // NOTE: For this case, the user can simply create a symlink
        // in its sandbox. No need for a volume.
        if (!path::is_absolute(volume.container_path())) {
          return Failure(
              "'container_path' is relative for "
              "SANDBOX_PATH volume SELF type");
        }

        sourceRoot = containerConfig.directory();
        source = path::join(sourceRoot, sandboxPath->path());
        break;
      case Volume::Source::SandboxPath::PARENT:
        if (!containerId.has_parent()) {
          return Failure(
              "SANDBOX_PATH volume PARENT type "
              "only works for nested container");
        }

        if (!sandboxes.contains(containerId.parent())) {
          return Failure(
              "Failed to locate the sandbox for the parent container");
        }

        sourceRoot = sandboxes[containerId.parent()];
        source = path::join(sourceRoot, sandboxPath->path());
        break;
      default:
        return Failure("Unknown SANDBOX_PATH volume type");
    }

    // NOTE: Chown should be avoided if the 'source' directory already
    // exists because it may be owned by some other user and should
    // not be mutated.
    if (!os::exists(source)) {
      Try<Nothing> mkdir = os::mkdir(source);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the directory '" + source + "' "
            "in the sandbox: " + mkdir.error());
      }

      // Get 'sourceRoot''s user and group info for the source path.
      struct stat s;
      if (::stat(sourceRoot.c_str(), &s) < 0) {
        return ErrnoFailure("Failed to stat '" + sourceRoot + "'");
      }

      LOG(INFO) << "Changing the ownership of the SANDBOX_PATH volume at '"
                << source << "' with UID " << s.st_uid << " and GID "
                << s.st_gid;

      Try<Nothing> chown = os::chown(s.st_uid, s.st_gid, source, false);
      if (chown.isError()) {
        return Failure(
            "Failed to change the ownership of the SANDBOX_PATH volume at '" +
            source + "' with UID " + stringify(s.st_uid) + " and GID " +
            stringify(s.st_gid) + ": " + chown.error());
      }
    }

    // Prepare the target.
    string target;

    if (path::is_absolute(volume.container_path())) {
      CHECK(bindMountSupported);

      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        if (os::stat::isdir(source)) {
          Try<Nothing> mkdir = os::mkdir(target);
          if (mkdir.isError()) {
            return Failure(
                "Failed to create the mount point at "
                "'" + target + "': " + mkdir.error());
          }
        } else {
          // The file (regular file or device file) bind mount case.
          Try<Nothing> mkdir = os::mkdir(Path(target).dirname());
          if (mkdir.isError()) {
            return Failure(
                "Failed to create directory "
                "'" + Path(target).dirname() + "' "
                "for the mount point: " + mkdir.error());
          }

          Try<Nothing> touch = os::touch(target);
          if (touch.isError()) {
            return Failure(
                "Failed to touch the mount point at "
                "'" + target + "': " + touch.error());
          }
        }
      } else {
        target = volume.container_path();

        // An absolute 'container_path' must already exist if the
        // container rootfs is the same as the host. This is because
        // we want to avoid creating mount points outside the work
        // directory in the host filesystem.
        if (!os::exists(target)) {
          return Failure(
              "Mount point '" + target + "' is an absolute path. "
              "It must exist if the container shares the host filesystem");
        }
      }

      // TODO(jieyu): We need to check that target resolves under
      // 'rootfs' because a user can potentially use a container path
      // like '/../../abc'.
    } else {
      CHECK_EQ(Volume::Source::SandboxPath::PARENT, sandboxPath->type());

      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            flags.sandbox_directory,
            volume.container_path());
      } else {
        target = path::join(
            containerConfig.directory(),
            volume.container_path());
      }

      // Create the mount point if bind mount is used.
      // NOTE: We cannot create the mount point at 'target' if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside 'target'. So we should always create
      // the mount point in the sandbox.
      if (bindMountSupported) {
        const string mountPoint = path::join(
            containerConfig.directory(),
            volume.container_path());

        if (os::stat::isdir(source)) {
          Try<Nothing> mkdir = os::mkdir(mountPoint);
          if (mkdir.isError()) {
            return Failure(
                "Failed to create the mount point at "
                "'" + mountPoint + "': " + mkdir.error());
          }
        } else {
          // The file (regular file or device file) bind mount case.
          Try<Nothing> mkdir = os::mkdir(Path(mountPoint).dirname());
          if (mkdir.isError()) {
            return Failure(
                "Failed to create the directory "
                "'" + Path(mountPoint).dirname() + "' "
                "for the mount point: " + mkdir.error());
          }

          Try<Nothing> touch = os::touch(mountPoint);
          if (touch.isError()) {
            return Failure(
                "Failed to touch the mount point at "
                "'" + mountPoint+ "': " + touch.error());
          }
        }
      }
    }

    if (bindMountSupported) {
#ifdef __linux__
      LOG(INFO) << "Mounting SANDBOX_PATH volume from "
                << "'" << source << "' to '" << target << "' "
                << "for container " << containerId;

      *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
          source,
          target,
          MS_BIND | MS_REC | (volume.mode() == Volume::RO ? MS_RDONLY : 0));

      // For the PARENT type SANDBOX_PATH volume, if the container's user is
      // not root and not the owner of the volume, call volume gid manager to
      // allocate a gid to make sure the container has the permission to access
      // the volume. Please note that we only do this when `bindMountSupported`
      // is true but not for the case of using symlink to do the SANDBOX_PATH
      // volume, because container's sandbox is created with 0750 permissions
      // (i.e., other users have no permissions, see MESOS-8332 for details), so
      // the nested container actually has no permissions to access anything
      // under its parent container's sandbox if their users are different,
      // that means the nested container cannot access the source path of the
      // volume (i.e., the source of the symlink) which is under its parent
      // container's sandbox.
      if (volumeGidManager &&
          containerConfig.has_user() &&
          containerConfig.user() != "root" &&
          sandboxPath->type() == Volume::Source::SandboxPath::PARENT) {
        Result<uid_t> uid = os::getuid(containerConfig.user());
        if (!uid.isSome()) {
          return Failure(
              "Failed to get the uid of user '" + containerConfig.user() + "': "
              + (uid.isError() ? uid.error() : "not found"));
        }

        struct stat s;
        if (::stat(source.c_str(), &s) < 0) {
          return ErrnoFailure("Failed to stat '" + source + "'");
        }

        if (uid.get() != s.st_uid) {
          LOG(INFO) << "Invoking volume gid manager to allocate gid to the "
                    << "volume path '" << source << "' for container "
                    << containerId;

          futures.push_back(
              volumeGidManager->allocate(source, VolumeGidInfo::SANDBOX_PATH));
        }
      }
#endif // __linux__
    } else {
      LOG(INFO) << "Linking SANDBOX_PATH volume from "
                << "'" << source << "' to '" << target << "' "
                << "for container " << containerId;

      // NOTE: We cannot enforce read-only access given the symlink without
      // changing the source so we just log a warning here.
      if (volume.mode() == Volume::RO) {
        LOG(WARNING) << "Allowing read-write access to read-only volume '"
                     << source << "' of container " << containerId;
      }

      Try<Nothing> symlink = ::fs::symlink(source, target);
      if (symlink.isError()) {
        return Failure(
            "Failed to symlink '" + source + "' -> '" + target + "'"
            ": " + symlink.error());
      }
    }
  }

  return collect(futures)
    .then([launchInfo](const vector<gid_t>& gids) mutable
        -> Future<Option<ContainerLaunchInfo>> {
      foreach (gid_t gid, gids) {
        launchInfo.add_supplementary_groups(gid);
      }

      return launchInfo;
    });
}


Future<Nothing> VolumeSandboxPathIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Remove the current container's sandbox path from `sandboxes`.
  sandboxes.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
