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

#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/mkdir.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/isolators/volume/sandbox_path.hpp"

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> VolumeSandboxPathIsolatorProcess::create(
    const Flags& flags)
{
  bool bindMountSupported = false;

  if (flags.launcher == "linux" &&
      strings::contains(flags.isolation, "filesystem/linux")) {
    bindMountSupported = true;
  }

  Owned<MesosIsolatorProcess> process(
      new VolumeSandboxPathIsolatorProcess(flags, bindMountSupported));

  return new MesosIsolator(process);
}


VolumeSandboxPathIsolatorProcess::VolumeSandboxPathIsolatorProcess(
    const Flags& _flags,
    bool _bindMountSupported)
  : ProcessBase(process::ID::generate("volume-sandbox-path-isolator")),
    flags(_flags),
    bindMountSupported(_bindMountSupported) {}


VolumeSandboxPathIsolatorProcess::~VolumeSandboxPathIsolatorProcess() {}


bool VolumeSandboxPathIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Nothing> VolumeSandboxPathIsolatorProcess::recover(
    const list<ContainerState>& states,
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
    return Failure(
        "Can only prepare the sandbox volume isolator "
        "for a MESOS container");
  }

  if (!bindMountSupported && containerConfig.has_rootfs()) {
    return Failure(
        "The 'linux' launcher and 'filesystem/linux' isolator must be "
        "enabled to change the rootfs and bind mount");
  }

  ContainerLaunchInfo launchInfo;

  foreach (const Volume& volume, containerInfo.volumes()) {
    if (!volume.has_source() ||
        !volume.source().has_type() ||
        volume.source().type() != Volume::Source::SANDBOX_PATH) {
      continue;
    }

    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return Failure(
          "SANDBOX_PATH volume is not supported for DEBUG containers");
    }

    if (!volume.source().has_sandbox_path()) {
      return Failure("volume.source.sandbox_path is not specified");
    }

    const Volume::Source::SandboxPath& sandboxPath =
      volume.source().sandbox_path();

    // TODO(jieyu): Support other type of SANDBOX_PATH (e.g., SELF).
    if (!sandboxPath.has_type() ||
        sandboxPath.type() != Volume::Source::SandboxPath::PARENT) {
      return Failure("Only PARENT sandbox path is supported");
    }

    if (!containerId.has_parent()) {
      return Failure("PARENT sandbox path only works for nested container");
    }

    // TODO(jieyu): Validate sandboxPath.path for other invalid chars.
    if (strings::contains(sandboxPath.path(), ".") ||
        strings::contains(sandboxPath.path(), " ")) {
      return Failure("Invalid char found in volume.source.sandbox_path.path");
    }

    if (!sandboxes.contains(containerId.parent())) {
      return Failure("Failed to locate the sandbox for the parent container");
    }

    // Prepare the source.
    const string source = path::join(
        sandboxes[containerId.parent()],
        sandboxPath.path());

    // NOTE: Chown should be avoided if the source directory already
    // exists because it may be owned by some other user and should
    // not be mutated.
    if (!os::exists(source)) {
      Try<Nothing> mkdir = os::mkdir(source);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the directory in the parent sandbox: " +
            mkdir.error());
      }

      // Get the parent sandbox user and group info for the source path.
      struct stat s;
      if (::stat(sandboxes[containerId.parent()].c_str(), &s) < 0) {
        return Failure(ErrnoError(
            "Failed to stat '" + sandboxes[containerId.parent()] + "'"));
      }

      LOG(INFO) << "Changing the ownership of the sandbox_path volume at '"
                << source << "' with UID " << s.st_uid << " and GID "
                << s.st_gid;

      Try<Nothing> chown = os::chown(s.st_uid, s.st_gid, source, false);
      if (chown.isError()) {
        return Failure(
            "Failed to change the ownership of the sandbox_path volume at '" +
            source + "' with UID " + stringify(s.st_uid) + " and GID " +
            stringify(s.st_gid) + ": " + chown.error());
      }
    }

    // Prepare the target.
    string target;

    if (path::absolute(volume.container_path())) {
      if (!bindMountSupported) {
        return Failure(
            "The 'linux' launcher and 'filesystem/linux' isolator must be "
            "enabled to support absolute container path");
      }

      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        Try<Nothing> mkdir = os::mkdir(target);
        if (mkdir.isError()) {
          return Failure(
              "Failed to create the target of the mount at '" +
              target + "': " + mkdir.error());
        }
      } else {
        target = volume.container_path();

        if (!os::exists(target)) {
          return Failure(
              "Absolute container path '" + target + "' "
              "does not exist");
        }
      }
    } else {
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

        Try<Nothing> mkdir = os::mkdir(mountPoint);
        if (mkdir.isError()) {
          return Failure(
              "Failed to create the target of the mount at '" +
              mountPoint + "': " + mkdir.error());
        }
      }
    }

    if (bindMountSupported) {
      LOG(INFO) << "Mounting SANDBOX_PATH volume from "
                << "'" << source << "' to '" << target << "' "
                << "for container " << containerId;

      CommandInfo* command = launchInfo.add_pre_exec_commands();
      command->set_shell(false);
      command->set_value("mount");
      command->add_arguments("mount");
      command->add_arguments("-n");
      command->add_arguments("--rbind");
      command->add_arguments(source);
      command->add_arguments(target);
    } else {
      LOG(INFO) << "Linking SANDBOX_PATH volume from "
                << "'" << source << "' to '" << target << "' "
                << "for container " << containerId;

      Try<Nothing> symlink = fs::symlink(source, target);
      if (symlink.isError()) {
        return Failure(
            "Failed to symlink '" + source + "' -> '" + target + "'"
            ": " + symlink.error());
      }
    }
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
