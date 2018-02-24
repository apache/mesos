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

#include "linux/ns.hpp"

#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"

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

  // For the container which wants to share pid namespace
  // with its parent, just return immediately.
  if (sharePidNamespace) {
    return launchInfo;
  }

  // For the container which does not want to share pid namespace with
  // its parent, make sure we will clone a new pid namespace for it.
  launchInfo.add_clone_namespaces(CLONE_NEWPID);

  // Mount /proc with standard options for the container's pid
  // namespace to show the container's pids (and other /proc files),
  // not the parent's. This technique was taken from unshare.c in
  // utils-linux for --mount-proc.
  //
  // NOTE: 'filesystem/linux' isolator will make sure mounts in the
  // child mount namespace will not be propagated back to the host
  // mount namespace.
  //
  // TOOD(jieyu): Consider unmount the existing /proc.
  ContainerMountInfo* mount = launchInfo.add_mounts();
  mount->set_source("proc");
  mount->set_target("/proc");
  mount->set_type("proc");
  mount->set_flags(MS_NOSUID | MS_NODEV | MS_NOEXEC);

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
