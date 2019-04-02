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

#include <process/id.hpp>

#include <stout/strings.hpp>

#include <stout/os/mkdir.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/ns.hpp"

#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerMountInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> NamespacesPidIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The pid namespace isolator requires root permissions");
  }

  // Verify that pid namespaces are available on this kernel.
  Try<bool> pidSupported = ns::supported(CLONE_NEWPID);
  if (pidSupported.isError() || !pidSupported.get()) {
    return Error("Pid namespaces are not supported by this kernel");
  }

  // Make sure 'linux' launcher is used because only 'linux' launcher
  // supports cloning pid namespace for the container.
  if (flags.launcher != "linux") {
    return Error("'linux' launcher must be used to enable pid namespace");
  }

  // Make sure 'filesystem/linux' isolator is used.
  // NOTE: 'filesystem/linux' isolator will make sure mounts in the
  // child mount namespace will not be propagated back to the host
  // mount namespace.
  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' must be used to enable pid namespace");
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new NamespacesPidIsolatorProcess(flags)));
}


NamespacesPidIsolatorProcess::NamespacesPidIsolatorProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("pid-namespace-isolator")),
    flags(_flags) {}


bool NamespacesPidIsolatorProcess::supportsNesting()
{
  return true;
}


bool NamespacesPidIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> NamespacesPidIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  ContainerLaunchInfo launchInfo;

  bool sharePidNamespace =
    containerConfig.container_info().linux_info().share_pid_namespace();

  if (containerId.has_parent()) {
    // If we are a nested container, then we want to enter our
    // parent's pid namespace before cloning a new one.
    launchInfo.add_enter_namespaces(CLONE_NEWPID);

    // For nested container in the `DEBUG` class, we don't want to clone a
    // new pid namespace at all, so we short circuit here.
    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return launchInfo;
    }
  } else {
    // If sharing agent pid namespace with top-level container is disallowed,
    // but the framework requests it by setting the `share_pid_namespace` field
    // to true, the container launch will be rejected.
    if (flags.disallow_sharing_agent_pid_namespace && sharePidNamespace) {
      return Failure(
          "Sharing agent pid namespace with "
          "top-level container is not allowed");
    }
  }

  if (!sharePidNamespace) {
    // For the container which does not want to share pid namespace with
    // its parent, make sure we will clone a new pid namespace for it.
    launchInfo.add_clone_namespaces(CLONE_NEWPID);

    // Since this container is guaranteed to have its own pid
    // namespace, we need to to mount /proc so container's pids can be
    // shown properly. We will not see EBUSY when doing the mount as
    // it won't be the same as the host /proc mount.
    //
    // NOTE: 'filesystem/linux' isolator will make sure mounts in the
    // child mount namespace will not be propagated back to the host
    // mount namespace.
    *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
        "proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC);
  } else {
    if (containerId.has_parent()) {
      // This container shares the same pid namespace as its parent
      // and is not a top level container. This means it might not
      // share the same pid namespace as the agent. In this case, we
      // will mount `/proc`. In the case where this container does
      // share the pid namespace of the agent (because its parent
      // shares the same pid namespace of the agent), mounting `/proc`
      // at the same place will result in EBUSY. As a result, we
      // always "move" (MS_MOVE) the mounts under `/proc` to a new
      // location and mount the `/proc` again at the old location. See
      // MESOS-9529 for details.
      //
      // TODO(jieyu): Consider unmount the old proc mounts.
      const string mountPoint =
        containerizer::paths::getHostProcMountPointPath(
            flags.runtime_dir,
            containerId);

      Try<Nothing> mkdir = os::mkdir(mountPoint);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create host proc mount point at "
            "'" + mountPoint + "': " + mkdir.error());
      }

      *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
          "/proc", mountPoint, MS_MOVE);
      *launchInfo.add_mounts() = protobuf::slave::createContainerMount(
          "proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC);
    }
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
