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

#ifndef __CGROUPS_ISOLATOR_HPP__
#define __CGROUPS_ISOLATOR_HPP__

#include <string>

#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/multihashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystem.hpp"

namespace mesos {
namespace internal {
namespace slave {

// This isolator manages all cgroups subsystems for containers, and delegate
// most operations on cgroups subsystem to specific `Subsystem` class.
class CgroupsIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~CgroupsIsolatorProcess() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

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

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;

    // This promise will complete if a container is impacted by a resource
    // limitation and should be terminated.
    process::Promise<mesos::slave::ContainerLimitation> limitation;

    // This `hashset` stores the name of subsystems which are recovered
    // or prepared for the container.
    hashset<std::string> subsystems;
  };

  CgroupsIsolatorProcess(
      const Flags& _flags,
      const multihashmap<std::string, process::Owned<Subsystem>>&
        _subsystems);

  process::Future<Nothing> _recover(
      const hashset<ContainerID>& orphans,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> __recover(
      const hashset<ContainerID>& unknownOrphans,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Nothing> ___recover(
      const ContainerID& containerId);

  process::Future<Nothing> ____recover(
      const ContainerID& containerId,
      const hashset<std::string>& recoveredSubsystems,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::vector<process::Future<Nothing>>& futures);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> __prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

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
      const std::vector<process::Future<Nothing>>& futures);

  process::Owned<Info> findCgroupInfo(const ContainerID& containerId) const;

  const Flags flags;

  // We map hierarchy path and `Subsystem` in subsystems. Same hierarchy may
  // map to multiple Subsystems. For example, our cgroups hierarchies may
  // mount like below in the machine:
  //   /cgroup/cpu,cpuacct -> cpu
  //   /cgroup/cpu,cpuacct -> cpuacct
  //   /cgroup/memory      -> memory
  // As we see, subsystem 'cpu' and 'cpuacct' are co-mounted at
  // '/cgroup/cpu,cpuacct'.
  multihashmap<std::string, process::Owned<Subsystem>> subsystems;

  // Store cgroups associated information for containers.
  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_ISOLATOR_HPP__
