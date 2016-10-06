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

  return new MesosIsolator(
      Owned<MesosIsolatorProcess>(new LinuxCapabilitiesIsolatorProcess(flags)));
}


Future<Option<ContainerLaunchInfo>> LinuxCapabilitiesIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // Determine the capabilities of the container that we want to
  // launch. None() here means that we don't want to set capabilities
  // for the container.
  Option<CapabilityInfo> capabilities = None();

  if (containerConfig.has_container_info() &&
      containerConfig.container_info().has_linux_info() &&
      containerConfig.container_info().linux_info().has_capability_info()) {
    capabilities =
      containerConfig.container_info().linux_info().capability_info();
  }

  // If both the framework sets the capabilities for the container and
  // the operator sets the allowed capabilities on the agent, we need
  // to verify that the request from the framework is allowed.
  if (capabilities.isSome() && flags.allowed_capabilities.isSome()) {
    const set<Capability> requested = convert(capabilities.get());
    const set<Capability> allowed = convert(flags.allowed_capabilities.get());

    if ((requested & allowed).size() != requested.size()) {
      return Failure(
          "Capabilities requested '" + stringify(requested) + "', "
          "but only '" + stringify(allowed) + "' are allowed");
    }
  }

  // If the framework does not set the capabilities and the operator
  // sets the allowed capabilities, use that as the capabilities for
  // the container.
  if (capabilities.isNone() && flags.allowed_capabilities.isSome()) {
    capabilities = flags.allowed_capabilities.get();
  }

  // If no capabilities need to be set for the container, we do not
  // need to modify the container launch info.
  if (capabilities.isNone()) {
    return None();
  }

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
          "--capabilities=" + stringify(JSON::protobuf(capabilities.get())));
    } else {
      launchInfo.mutable_capabilities()->CopyFrom(capabilities.get());
    }
  } else {
    // Custom executor or nested container.
    launchInfo.mutable_capabilities()->CopyFrom(capabilities.get());
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
