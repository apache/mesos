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

#include <sys/prctl.h>

#include <stout/path.hpp>

#include "linux/seccomp/seccomp_parser.hpp"

#include "slave/containerizer/mesos/isolators/linux/seccomp.hpp"

using process::Failure;
using process::Future;
using process::Owned;

using mesos::seccomp::ContainerSeccompProfile;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

// NOTE: The definition below was taken from the Linux Kernel sources.
//
// TODO(abudnik): This definition should be removed in favor of using
// `linux/seccomp.h` once we drop support for kernels older than 3.5.
#if !defined(SECCOMP_MODE_FILTER)
#define SECCOMP_MODE_FILTER 2
#endif

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxSeccompIsolatorProcess::create(const Flags& flags)
{
  if (geteuid() != 0) {
    return Error("Linux Seccomp isolator requires root permissions");
  }

  // Check if the kernel supports seccomp filter.
  const int ret = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, nullptr);
  if (!(ret == -1 && EFAULT == errno)) {
    return Error("Seccomp is not supported by the kernel");
  }

  if (flags.seccomp_config_dir.isNone()) {
    return Error("Missing required `--seccomp_config_dir` flag");
  }

  Option<ContainerSeccompProfile> defaultProfile;

  // Parse default Seccomp profile.
  if (flags.seccomp_profile_name.isSome()) {
    const auto path = path::join(
        flags.seccomp_config_dir.get(), flags.seccomp_profile_name.get());

    Try<ContainerSeccompProfile> profile =
      mesos::internal::seccomp::parseProfile(path);

    if (profile.isError()) {
      return Error(profile.error());
    }

    defaultProfile = profile.get();
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new LinuxSeccompIsolatorProcess(flags, defaultProfile)));
}


bool LinuxSeccompIsolatorProcess::supportsNesting() { return true; }
bool LinuxSeccompIsolatorProcess::supportsStandalone() { return true; }


Future<Option<ContainerLaunchInfo>> LinuxSeccompIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  Option<ContainerSeccompProfile> profile = defaultProfile;

  std::string profileName =
    flags.seccomp_profile_name.isSome() ? flags.seccomp_profile_name.get() : "";

  // Framework can override default Seccomp profile for a particular container.
  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_linux_info() &&
      containerConfig.container_info().linux_info().has_seccomp()) {
    const auto& seccomp =
      containerConfig.container_info().linux_info().seccomp();

    const bool unconfined =
      seccomp.has_unconfined() ? seccomp.unconfined() : false;

    // Validate Seccomp configuration.
    if (unconfined && seccomp.has_profile_name()) {
      return Failure(
          "Invalid Seccomp configuration: 'profile_name' given even "
          "though 'unconfined' Seccomp setting is enabled");
    }

    if (seccomp.has_profile_name()) {
      profileName = seccomp.profile_name();
      const auto path = path::join(flags.seccomp_config_dir.get(), profileName);

      Try<ContainerSeccompProfile> customProfile =
        mesos::internal::seccomp::parseProfile(path);

      if (customProfile.isError()) {
        return Failure(customProfile.error());
      }

      profile = customProfile.get();
    } else if (unconfined) {
      LOG(INFO) << "Seccomp is not applied to container " << containerId;

      return None();
    } else {
      return Failure("Missing Seccomp profile name");
    }
  }

  if (profile.isNone()) {
    return None();
  }

  ContainerLaunchInfo launchInfo;
  launchInfo.mutable_seccomp_profile()->CopyFrom(profile.get());

  LOG(INFO) << "Using Seccomp profile '" << profileName
            << "' for container " << containerId;

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
