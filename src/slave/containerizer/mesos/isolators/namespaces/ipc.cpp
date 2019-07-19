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

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/os.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/namespaces/ipc.hpp"

using process::Failure;
using process::Future;

using std::string;

using mesos::internal::protobuf::slave::createContainerMount;

using mesos::internal::slave::containerizer::paths::AGENT_SHM_DIRECTORY;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> NamespacesIPCIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The IPC namespace isolator requires root permissions");
  }

  // Verify that IPC namespaces are available on this kernel.
  Try<bool> ipcSupported = ns::supported(CLONE_NEWIPC);
  if (ipcSupported.isError() || !ipcSupported.get()) {
    return Error("IPC namespaces are not supported by this kernel");
  }

  // Make sure the 'linux' launcher is used because only 'linux' launcher
  // supports cloning namespaces for the container.
  if (flags.launcher != "linux") {
    return Error(
        "The 'linux' launcher must be used to enable the IPC namespace");
  }

  // Make sure 'filesystem/linux' isolator is used.
  // NOTE: 'filesystem/linux' isolator will make sure mounts in the
  // child mount namespace will not be propagated back to the host
  // mount namespace.
  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' must be used to enable IPC namespace");
  }

  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
      new NamespacesIPCIsolatorProcess(flags)));
}


NamespacesIPCIsolatorProcess::NamespacesIPCIsolatorProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("ipc-namespace-isolator")),
    flags(_flags) {}


bool NamespacesIPCIsolatorProcess::supportsNesting()
{
  return true;
}


bool NamespacesIPCIsolatorProcess::supportsStandalone()
{
  return true;
}


// IPC isolation on Linux just requires that a process be placed in an IPC
// namespace. Neither /proc, nor any of the special SVIPC filesystem need
// to be remounted for this to work. IPC namespaces are disjoint. That is,
// once you enter an IPC namespace, IPC objects from the host namespace are
// no longer visible (and vice versa).
Future<Option<ContainerLaunchInfo>> NamespacesIPCIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  ContainerLaunchInfo launchInfo;
  Option<LinuxInfo::IpcMode> ipcMode;
  Option<Bytes> shmSize;

  // Get the container's IPC mode and size of /dev/shm.
  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_linux_info()) {
    if (containerConfig.container_info().linux_info().has_ipc_mode()) {
      ipcMode = containerConfig.container_info().linux_info().ipc_mode();
    }

    if (containerConfig.container_info().linux_info().has_shm_size()) {
      if (ipcMode != LinuxInfo::PRIVATE) {
        return Failure(
            "Only support specifying the size of /dev/shm "
            "when the IPC mode is `PRIVATE`");
      }

      shmSize =
        Megabytes(containerConfig.container_info().linux_info().shm_size());
    } else if (flags.default_container_shm_size.isSome()) {
      shmSize = flags.default_container_shm_size.get();
    }
  }

  if (containerId.has_parent()) {
    // Debug container always shares its parent container's IPC namespace
    // and /dev/shm. Please note that `filesystem/linux` isolator will
    // ensure debug container enters its parent container's mount namespace
    // so it will share its parent container's /dev/shm.
    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      if (ipcMode == LinuxInfo::PRIVATE) {
        return Failure(
            "Private IPC mode is not supported for DEBUG containers");
      }

      launchInfo.add_enter_namespaces(CLONE_NEWIPC);
      return launchInfo;
    }

    if (ipcMode.isNone()) {
      // If IPC mode is not set, for backward compatibility we will keep the
      // previous behavior: Nested container will share the IPC namespace from
      // its parent container, and if it does not have its own rootfs, it will
      // share agent's /dev/shm, otherwise it will have its own /dev/shm.
      launchInfo.add_enter_namespaces(CLONE_NEWIPC);

      if (containerConfig.has_rootfs()) {
        *launchInfo.add_mounts() = createContainerMount(
            "tmpfs",
            path::join(containerConfig.rootfs(), "/dev/shm"),
            "tmpfs",
            "mode=1777",
            MS_NOSUID | MS_NODEV | MS_STRICTATIME);
      }
    } else {
      switch (ipcMode.get()) {
        case LinuxInfo::PRIVATE: {
          // If IPC mode is `PRIVATE`, nested container will have its own
          // IPC namespace and /dev/shm.
          launchInfo.add_clone_namespaces(CLONE_NEWIPC);

          // Create a tmpfs mount in agent host for nested container's /dev/shm.
          const string shmPath = containerizer::paths::getContainerShmPath(
              flags.runtime_dir, containerId);

          Try<Nothing> mkdir = os::mkdir(shmPath);
          if (mkdir.isError()) {
            return Failure(
                "Failed to create container shared memory directory: " +
                mkdir.error());
          }

          Try<Nothing> mnt = fs::mount(
              "tmpfs",
              shmPath,
              "tmpfs",
              MS_NOSUID | MS_NODEV | MS_STRICTATIME,
              shmSize.isSome() ?
                strings::format("mode=1777,size=%d", shmSize->bytes()).get() :
                "mode=1777");

          if (mnt.isError()) {
            return Failure("Failed to mount '" + shmPath + "': " + mnt.error());
          }

          // Bind mount the tmpfs mount at /dev/shm in nested container's mount
          // namespace.
          *launchInfo.add_mounts() = createContainerMount(
              shmPath,
              containerConfig.has_rootfs()
                ? path::join(containerConfig.rootfs(), "/dev/shm")
                : "/dev/shm",
              MS_BIND);

          break;
        }
        case LinuxInfo::SHARE_PARENT: {
          // If IPC mode is `SHARE_PARENT`, nested container will its parent
          // container's IPC namespace and /dev/shm.
          launchInfo.add_enter_namespaces(CLONE_NEWIPC);

          Try<string> parentShmPath = containerizer::paths::getParentShmPath(
              flags.runtime_dir,
              containerId);

          if (parentShmPath.isError()) {
            return Failure(
                "Failed to get parent shared memory path: " +
                parentShmPath.error());
          } else if (parentShmPath.get() != AGENT_SHM_DIRECTORY ||
                     containerConfig.has_rootfs()) {
            // To share parent container's /dev/shm, we need to bind mount
            // parent container's /dev/shm at /dev/shm in nested container's
            // mount namespace. Please note that we do not need to do this if
            // the parent container uses agent's /dev/shm and the nested
            // container does not has its own rootfs in which case the nested
            // container can directly access the agent's /dev/shm.
            *launchInfo.add_mounts() = createContainerMount(
                parentShmPath.get(),
                containerConfig.has_rootfs()
                  ? path::join(containerConfig.rootfs(), "/dev/shm")
                  : "/dev/shm",
                MS_BIND);
          }

          break;
        }
        case LinuxInfo::UNKNOWN: {
          return Failure("Unknown IPC mode");
        }
      }
    }
  } else {
    // This is the case of top-level container.
    if (ipcMode.isNone()) {
      // If IPC mode is not set, for backward compatibility we will keep the
      // previous behavior: Top-level container will have its own IPC namespace,
      // and if it does not have its own rootfs, it will share agent's /dev/shm,
      // otherwise it will have its own /dev/shm.
      launchInfo.add_clone_namespaces(CLONE_NEWIPC);

      if (containerConfig.has_rootfs()) {
        *launchInfo.add_mounts() = createContainerMount(
            "tmpfs",
            path::join(containerConfig.rootfs(), "/dev/shm"),
            "tmpfs",
            "mode=1777",
            MS_NOSUID | MS_NODEV | MS_STRICTATIME);
      }
    } else {
      switch (ipcMode.get()) {
        case LinuxInfo::PRIVATE: {
          // If IPC mode is `PRIVATE`, top-level container will have its own
          // IPC namespace and /dev/shm.
          launchInfo.add_clone_namespaces(CLONE_NEWIPC);

          // Create a tmpfs mount in agent host for top-level container's
          // /dev/shm.
          const string shmPath = containerizer::paths::getContainerShmPath(
              flags.runtime_dir, containerId);

          Try<Nothing> mkdir = os::mkdir(shmPath);
          if (mkdir.isError()) {
            return Failure(
                "Failed to create container shared memory directory: " +
                mkdir.error());
          }

          Try<Nothing> mnt = fs::mount(
              "tmpfs",
              shmPath,
              "tmpfs",
              MS_NOSUID | MS_NODEV | MS_STRICTATIME,
              shmSize.isSome() ?
                strings::format("mode=1777,size=%d", shmSize->bytes()).get() :
                "mode=1777");

          if (mnt.isError()) {
            return Failure("Failed to mount '" + shmPath + "': " + mnt.error());
          }

          // Bind mount the tmpfs mount at /dev/shm in top-level container's
          // mount namespace.
          *launchInfo.add_mounts() = createContainerMount(
              shmPath,
              containerConfig.has_rootfs() ?
                path::join(containerConfig.rootfs(), "/dev/shm") :
                "/dev/shm",
              MS_BIND);

          break;
        }
        case LinuxInfo::SHARE_PARENT: {
          if (flags.disallow_sharing_agent_ipc_namespace) {
            return Failure(
                "Sharing agent IPC namespace with "
                "top-level container is not allowed");
          }

          // If top-level container has its own rootfs, we will bind mount
          // agent's /dev/shm at /dev/shm in its mount namespace, otherwise
          // we do not need to anything since it can directly access agent's
          // /dev/shm.
          if (containerConfig.has_rootfs()) {
            *launchInfo.add_mounts() = createContainerMount(
                AGENT_SHM_DIRECTORY,
                containerConfig.has_rootfs() ?
                  path::join(containerConfig.rootfs(), "/dev/shm") :
                  "/dev/shm",
                MS_BIND);
          }

          break;
        }
        case LinuxInfo::UNKNOWN: {
          return Failure("Unknown IPC mode");
        }
      }
    }
  }

  return launchInfo;
}


Future<Nothing> NamespacesIPCIsolatorProcess::cleanup(
      const ContainerID& containerId)
{
  const string shmPath = containerizer::paths::getContainerShmPath(
      flags.runtime_dir, containerId);

  if (os::exists(shmPath)) {
    Try<Nothing> unmount = fs::unmount(shmPath);
    if (unmount.isError()) {
      return Failure(
          "Failed to unmount container shared memory directory '" +
          shmPath + "': " + unmount.error());
    }
  }

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
