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

#include <unistd.h>

#include <sys/types.h>

#include <algorithm>
#include <set>

#include <stout/set.hpp>

#include "linux/capabilities.hpp"

#include "slave/containerizer/mesos/isolators/linux/capabilities.hpp"

using std::set;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::convert;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxCapabilitiesIsolatorProcess::create(const Flags& flags)
{
  if (geteuid() != 0) {
    return Error("Linux capabilities isolator requires root permissions");
  }

  Try<Capabilities> create = Capabilities::create();
  if (create.isError()) {
    return Error("Failed to initialize capabilities: " + create.error());
  }

  if (flags.effective_capabilities.isSome() &&
      flags.bounding_capabilities.isSome()) {
    const set<Capability> bounding = convert(
        flags.bounding_capabilities.get());
    const set<Capability> effective = convert(
        flags.effective_capabilities.get());

    if ((effective & bounding).size() != effective.size()) {
      return Error(
          "Allowed capabilities are not a subset of the bounding capabilites");
    }
  }

  return new MesosIsolator(
      Owned<MesosIsolatorProcess>(new LinuxCapabilitiesIsolatorProcess(flags)));
}


bool LinuxCapabilitiesIsolatorProcess::supportsNesting()
{
  return true;
}


bool LinuxCapabilitiesIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> LinuxCapabilitiesIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  Option<CapabilityInfo> effective = None();
  Option<CapabilityInfo> bounding = None();

  // If effective capabilities are specified, those become
  // both the effective and bounding sets, because we guarantee
  // that the effective set is at least as restrictive as the
  // bounding set.
  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_linux_info()) {
    const auto& linuxInfo = containerConfig.container_info().linux_info();

    if (linuxInfo.has_capability_info() &&
        linuxInfo.has_effective_capabilities()) {
      return Failure(
          "Only one of 'capability_info' or 'effective_capabilities' "
          "is allowed");
    }

    if (linuxInfo.has_capability_info()) {
      effective = linuxInfo.capability_info();
    }

    if (linuxInfo.has_effective_capabilities()) {
      effective = linuxInfo.effective_capabilities();
    }

    if (linuxInfo.has_bounding_capabilities()) {
      bounding = linuxInfo.bounding_capabilities();
    }
  }

  // If the framework didn't specify, use the operator effective set.
  if (effective.isNone()) {
    effective = flags.effective_capabilities;
  }

  // If the framework specified a bounding set, test it against
  // flags.bounding_capabilities since that defines the limits of
  // what the operator is willing to allow.
  if (bounding.isSome() && flags.bounding_capabilities.isSome()) {
    const set<Capability> requested = convert(bounding.get());
    const set<Capability> allowed = convert(flags.bounding_capabilities.get());

    if ((requested & allowed).size() != requested.size()) {
      return Failure(
          "Bounding capabilities '" + stringify(requested) + "', "
          "but only '" + stringify(allowed) + "' are allowed");
    }
  }

  // If the framework didn't specify, use the operator bounding set and fall
  // back to the effective set if necessary.
  if (bounding.isNone()) {
    bounding = flags.bounding_capabilities;
  }

  if (effective.isSome() && bounding.isNone()) {
    bounding = effective;
  }

  // Require the effective task capabilities to be within the bounding set.
  if (effective.isSome()) {
    CHECK_SOME(bounding);

    const set<Capability> requested = convert(effective.get());
    const set<Capability> allowed = convert(bounding.get());

    if ((requested & allowed).size() != requested.size()) {
      return Failure(
          "Requested capabilities '" + stringify(requested) + "', "
          "but only '" + stringify(allowed) + "' are allowed");
    }
  }

  // If no capabilities need to be set for the container, we do
  // not need to modify the container launch info.
  if (effective.isNone() && bounding.isNone()) {
    return None();
  }

  // The bounding set is always present, but the effective set
  // may be absent.
  CHECK_SOME(bounding);

  ContainerLaunchInfo launchInfo;

  if (containerConfig.has_task_info()) {
    // Command task case.
    // 1) If the command task specifies a root filesystem, we need the
    //    command executor to be run with full privileges to perform
    //    operations like 'pivot_root'. Therefore, we set the command
    //    executor flags and the command executor will set the
    //    capabilities when it launches the task.
    // 2) If the command task does not specify a root filesystem, we
    //    will set the capabilities before we execute the command
    //    executor. Command executor itself does not require any
    //    capabilities to execute the user task in that case.
    //
    // TODO(jieyu): The caveat in case 2) is that the command executor
    // should be executable after the process drops capabilities.  For
    // containers that want to run under root, if the requested
    // capability for the container does not have CAP_DAC_READ_SEARCH,
    // the exec will fail with EACCESS, which is not the typical
    // behavior a user would expect from a root user.
    if (containerConfig.has_rootfs()) {
      launchInfo.mutable_command()->add_arguments(
          "--bounding_capabilities=" +
          stringify(JSON::protobuf(bounding.get())));
      if (effective.isSome()) {
        launchInfo.mutable_command()->add_arguments(
            "--effective_capabilities=" +
            stringify(JSON::protobuf(effective.get())));
      }
    } else {
      launchInfo.mutable_bounding_capabilities()->CopyFrom(bounding.get());
      if (effective.isSome()) {
        launchInfo.mutable_effective_capabilities()->CopyFrom(effective.get());
      }
    }
  } else {
    // Custom executor or nested container.
    launchInfo.mutable_bounding_capabilities()->CopyFrom(bounding.get());
    if (effective.isSome()) {
      launchInfo.mutable_effective_capabilities()->CopyFrom(effective.get());
    }
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
