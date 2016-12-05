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

#include "linux/ns.hpp"

#include "slave/containerizer/mesos/isolators/namespaces/ipc.hpp"

using process::Future;

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
  if (ns::namespaces().count("ipc") == 0) {
    return Error("IPC namespaces are not supported by this kernel");
  }

  // Make sure the 'linux' launcher is used because only 'linux' launcher
  // supports cloning namespaces for the container.
  if (flags.launcher != "linux") {
    return Error(
        "The 'linux' launcher must be used to enable the IPC namespace");
  }

  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
      new NamespacesIPCIsolatorProcess()));
}


NamespacesIPCIsolatorProcess::NamespacesIPCIsolatorProcess()
  : ProcessBase(process::ID::generate("ipc-namespace-isolator")) {}


bool NamespacesIPCIsolatorProcess::supportsNesting()
{
  return true;
}


// IPC isolation on Linux just requires that a process be placed in an IPC
// namespace. Neither /proc, nor any of the special SVIPC filesystem need
// to be remounted for this to work. IPC namespaces are disjoint. That is,
// once you enter an IPC namespace, IPC objects from the host namespace are
// no longer visible (and vice versa). Since IPC namespaces do not nest,
// we always place nested containers into the IPC namespace of the parent
// container. That is, containers in the same group share an IPC namespace,
// but groups are isolated from each other.
Future<Option<ContainerLaunchInfo>> NamespacesIPCIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  ContainerLaunchInfo launchInfo;

  if (containerId.has_parent()) {
    launchInfo.add_enter_namespaces(CLONE_NEWIPC);
  } else {
    launchInfo.add_clone_namespaces(CLONE_NEWIPC);
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
