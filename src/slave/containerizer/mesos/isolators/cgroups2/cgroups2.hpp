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

#ifndef __CGROUPS_V2_ISOLATOR_HPP__
#define __CGROUPS_V2_ISOLATOR_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/device_manager/device_manager.hpp"
#include "slave/containerizer/mesos/isolator.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controller.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Cgroups v2 Mesos isolator.
//
// Manages the cgroup v2 controllers that are used by containers. Each
// container is associated with two cgroups: a non-leaf cgroup whose control
// files are updated and a leaf cgroup where the container's processes lives.
// The container pid cannot live in the non-leaf cgroup because of the cgroups
// v2 internal process constraint:
//
// https://docs.kernel.org/admin-guide/cgroup-v2.html#no-internal-process-constraint // NOLINT
//
// Example cgroups:
//
//       containerA                       non-leaf cgroup
//       /      \                         /             |
//   processes  containerB           leaf cgroup   non-leaf child cgroup
//               |                                      |
//              processes                          leaf-cgroup
//
// TODO(dleamy): Nested containers are not yet supported.
class Cgroups2IsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      const process::Owned<DeviceManager>& deviceManager);

  ~Cgroups2IsolatorProcess() override;

  bool supportsNesting() override;

  bool supportsStandalone() override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<ContainerStatus> status(
      const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(const ContainerID& containerId) override;
private:
  struct Info
  {
    Info(const ContainerID& containerId,
         const std::string& cgroup,
         const std::string& cgroup_leaf,
         const bool isolate)
      : containerId(containerId), cgroup(cgroup), cgroup_leaf(cgroup_leaf), isolate(isolate) {}

    const ContainerID containerId;

    // Non-leaf cgroup for the container. Control files in this cgroup are
    // updated to set resource constraints on this and descendant
    // containers. Processes should not be assigned to this cgroup.
    const std::string cgroup;
    const std::string cgroup_leaf;

    // Names of the controllers which are prepared for the container.
    hashset<std::string> controllers;

    // Promise that will complete when a container is impacted by a resource
    // limitation and should be terminated.
    process::Promise<mesos::slave::ContainerLimitation> limitation;

    // Whether to perform resource isolation on this container.
    //   1. For non-nested containers, this will always be true.
    //   2. For nested containers, this may be true or false.
    //
    // This field is derived from LinuxInfo::share_cgroups.
    const bool isolate;
  };

  Cgroups2IsolatorProcess(
      const Flags& flags,
      const hashmap<std::string, process::Owned<Controller>>& controllers,
      const process::Owned<DeviceManager>& deviceManager);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig,
    const std::vector<process::Future<Nothing>>& futures);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> __prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  process::Future<Nothing> _recover(
    const hashset<ContainerID>& orphans);

  process::Future<Nothing> __recover(
      const hashset<ContainerID>& unknownOrphans,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> ___recover(
      const ContainerID& containerId,
      bool isolate = true);

  process::Future<Nothing> ____recover(
      const ContainerID& containerId,
      const hashset<std::string>& recoveredSubsystems,
      bool isolate,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> _isolate(
      const std::vector<process::Future<Nothing>>& futures,
      const ContainerID& containerId,
      pid_t pid);

  void _watch(
    const ContainerID& containerId,
    const process::Future<mesos::slave::ContainerLimitation>& future);

  process::Future<Nothing> _update(
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> __cleanup(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  process::Owned<Cgroups2IsolatorProcess::Info> cgroupInfo(
      const ContainerID& containerId) const;

  Flags flags;

  // Maps each controller to the `Controller` isolator that manages it.
  hashmap<std::string, process::Owned<Controller>> controllers;

  // Associates a container with the information to access its controllers.
  hashmap<ContainerID, process::Owned<Info>> infos;

  const process::Owned<DeviceManager> deviceManager;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif