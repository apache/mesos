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

#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/touch.hpp>

#include "common/protobuf_utils.hpp"
#include "common/validation.hpp"

#include "linux/fs.hpp"

#include "slave/containerizer/mesos/isolators/volume/host_path.hpp"
#include "slave/containerizer/mesos/isolators/volume/utils.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::MountPropagation;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerMountInfo;
using mesos::slave::Isolator;

using mesos::internal::slave::volume::PathValidator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> VolumeHostPathIsolatorProcess::create(
    const Flags& flags)
{
  if (flags.launcher != "linux") {
    return Error("'linux' launcher must be used");
  }

  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' isolator must be used");
  }

  Owned<MesosIsolatorProcess> process(
      flags.host_path_volume_force_creation.isSome()
      ? new VolumeHostPathIsolatorProcess(
            flags,
            PathValidator::parse(flags.host_path_volume_force_creation.get()))
      : new VolumeHostPathIsolatorProcess(flags));

  return new MesosIsolator(process);
}


VolumeHostPathIsolatorProcess::VolumeHostPathIsolatorProcess(
    const Flags& _flags)
  : ProcessBase(process::ID::generate("volume-host-path-isolator")),
    flags(_flags),
    pathValidator() {}


VolumeHostPathIsolatorProcess::VolumeHostPathIsolatorProcess(
    const Flags& _flags,
    const PathValidator& _pathValidator)
  : ProcessBase(process::ID::generate("volume-host-path-isolator")),
    flags(_flags),
    pathValidator(_pathValidator) {}


VolumeHostPathIsolatorProcess::~VolumeHostPathIsolatorProcess() {}


bool VolumeHostPathIsolatorProcess::supportsNesting()
{
  return true;
}


bool VolumeHostPathIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> VolumeHostPathIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  const ContainerInfo& containerInfo = containerConfig.container_info();

  if (containerInfo.type() != ContainerInfo::MESOS) {
    return Failure("Only support MESOS containers");
  }

  ContainerLaunchInfo launchInfo;

  foreach (const Volume& volume, containerInfo.volumes()) {
    // NOTE: The validation here is for backwards compatibility. For
    // example, if an old master (no validation code) is used to
    // launch a task with a volume.
    Option<Error> error = common::validation::validateVolume(volume);
    if (error.isSome()) {
      return Failure("Invalid volume: " + error->message);
    }

    Option<string> hostPath;
    bool mountPropagationBidirectional = false;

    // NOTE: This is the legacy way of specifying the Volume. The
    // 'host_path' can be relative in legacy mode, representing
    // SANDBOX_PATH volumes.
    if (volume.has_host_path() &&
        path::is_absolute(volume.host_path())) {
      hostPath = volume.host_path();
    }

    if (volume.has_source() &&
        volume.source().has_type() &&
        volume.source().type() == Volume::Source::HOST_PATH) {
      CHECK(volume.source().has_host_path());

      const Volume::Source::HostPath& hostPathInfo =
        volume.source().host_path();

      if (!path::is_absolute(hostPathInfo.path())) {
        return Failure(
            "Path '" + hostPathInfo.path() + "' "
            "in HOST_PATH volume is not absolute");
      }

      hostPath = hostPathInfo.path();

      mountPropagationBidirectional =
        hostPathInfo.has_mount_propagation() &&
        hostPathInfo.mount_propagation().mode() ==
          MountPropagation::BIDIRECTIONAL;
    }

    if (hostPath.isNone()) {
      continue;
    }

    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return Failure("HOST_PATH volume is not supported for DEBUG containers");
    }

    if (!os::exists(hostPath.get())) {
      if (pathValidator.isNone()) {
        return Failure(
            "Path '" + hostPath.get() + "' in HOST_PATH volume does not exist");
      }

      Try<string> normalizedPath = path::normalize(hostPath.get());
      if (normalizedPath.isError()) {
        return Failure(
            "Failed to normalized the host path '" + hostPath.get() + "': " +
            normalizedPath.error());
      }

      Try<Nothing> validate = pathValidator->validate(normalizedPath.get());
      if (validate.isError()) {
        return Failure(
            "Path '" + hostPath.get() + "' in HOST_PATH volume does not exist "
            "and is not whitelisted for creation: " + validate.error());
      }

      // Always assume the non-existing host path as a directory.
      Try<Nothing> mkdir = os::mkdir(normalizedPath.get());
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the host path at '" + hostPath.get() + "': " +
            mkdir.error());
      }
    }

    // Determine the mount point for the host volume.
    string mountPoint;

    if (path::is_absolute(volume.container_path())) {
      // TODO(jieyu): We need to check that the mount point resolves
      // under 'rootfs' because a user can potentially use a container
      // path like '/../../abc'.

      if (containerConfig.has_rootfs()) {
        mountPoint = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        if (os::stat::isdir(hostPath.get())) {
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
                "Failed to create directory "
                "'" + Path(mountPoint).dirname() + "' "
                "for the mount point: " + mkdir.error());
          }

          Try<Nothing> touch = os::touch(mountPoint);
          if (touch.isError()) {
            return Failure(
                "Failed to touch the mount point at "
                "'" + mountPoint + "': " + touch.error());
          }
        }
      } else {
        mountPoint = volume.container_path();

        // An absolute 'container_path' must already exist if the
        // container rootfs is the same as the host. This is because
        // we want to avoid creating mount points outside the work
        // directory in the host filesystem.
        if (!os::exists(mountPoint)) {
          return Failure(
              "Mount point '" + mountPoint + "' is an absolute path. "
              "It must exist if the container shares the host filesystem");
        }
      }
    } else {
      // TODO(jieyu): We need to check that the mount point resolves
      // under the sandbox because a user can potentially use a
      // container path like '../../abc'.

      // NOTE: If the container has its own rootfs, we cannot create
      // the mount point in the mapped sandbox location in container's
      // rootfs because the bind mount of the sandbox itself will hide
      // the mount point. So we should always create the mount point
      // in 'directory' first.
      mountPoint = path::join(
          containerConfig.directory(),
          volume.container_path());

      if (os::stat::isdir(hostPath.get())) {
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

      if (containerConfig.has_rootfs()) {
        mountPoint = path::join(
            containerConfig.rootfs(),
            flags.sandbox_directory,
            volume.container_path());
      }
    }

    if (mountPropagationBidirectional) {
      // First, find the mount entry that is the parent of the host
      // volume source. If it is not a shared mount, return a failure.
      Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
      if (table.isError()) {
        return Failure("Failed to read mount table: " + table.error());
      }

      Try<fs::MountInfoTable::Entry> sourceMountEntry =
        table->findByTarget(hostPath.get());

      if (sourceMountEntry.isError()) {
        return Failure(
            "Cannot find the mount containing host path '" +
            hostPath.get() + "': " + sourceMountEntry.error());
      }

      if (sourceMountEntry->shared().isNone()) {
        return Failure(
            "Cannot setup bidirectional mount propagation for host path '" +
            hostPath.get() + "' because it is not under a shared mount");
      }

      LOG(INFO) << "Mark '" << sourceMountEntry->target
                << "' as shared for container " << containerId;

      // This tells the launch helper to NOT mark the mount as slave
      // (otherwise, the propagation won't work).
      ContainerMountInfo* mount = launchInfo.add_mounts();
      mount->set_target(sourceMountEntry->target);
      mount->set_flags(MS_SHARED);
    }

    // NOTE: 'hostPath' and 'mountPoint' are equal only when the
    // container does not define its own image and shares the host
    // filesystem (otherwise, the mount point should be under
    // container's rootfs, which won't be equal to 'hostPath'). As a
    // result, no need for the bind mount because the 'hostPath' is
    // already accessible in the container.
    if (hostPath.get() != mountPoint) {
      *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
          hostPath.get(),
          mountPoint,
          MS_BIND | MS_REC | (volume.mode() == Volume::RO ? MS_RDONLY : 0));
    }
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
