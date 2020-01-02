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

#ifndef __CGROUPS_ISOLATOR_SUBSYSTEMS_MEMORY_HPP__
#define __CGROUPS_ISOLATOR_SUBSYSTEMS_MEMORY_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystem.hpp"

namespace mesos {
namespace internal {
namespace slave {

/**
 * Represent cgroups memory subsystem.
 */
class MemorySubsystemProcess : public SubsystemProcess
{
public:
  static Try<process::Owned<SubsystemProcess>> create(
      const Flags& flags,
      const std::string& hierarchy);

  ~MemorySubsystemProcess() override = default;

  std::string name() const override
  {
    return CGROUP_SUBSYSTEM_MEMORY_NAME;
  }

  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const std::string& cgroup,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      const std::string& cgroup,
      pid_t pid) override;

  process::Future<Nothing> recover(
      const ContainerID& containerId,
      const std::string& cgroup) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId,
      const std::string& cgroup) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const std::string& cgroup,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId,
      const std::string& cgroup) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId,
      const std::string& cgroup) override;

private:
  struct Info
  {
    // Used to cancel the OOM listening.
    process::Future<Nothing> oomNotifier;

    hashmap<
        cgroups::memory::pressure::Level,
        process::Owned<cgroups::memory::pressure::Counter>> pressureCounters;

    process::Promise<mesos::slave::ContainerLimitation> limitation;

    // Indicate whether the memory hard limit of this container has
    // already been updated.
    bool hardLimitUpdated;

    // Indicates whether this is a command task container. Please note
    // that we only need to use this field in isolating phase, so we do
    // not recover it after agent restarts, that means its value may not
    // be correct after agent recovery.
    bool isCommandTask;
  };

  MemorySubsystemProcess(const Flags& flags, const std::string& hierarchy);

  process::Future<ResourceStatistics> _usage(
      const ContainerID& containerId,
      ResourceStatistics result,
      const std::vector<cgroups::memory::pressure::Level>& levels,
      const std::vector<process::Future<uint64_t>>& values);

  // Start listening on OOM events. This function will create an
  // eventfd and start polling on it.
  void oomListen(
      const ContainerID& containerId,
      const std::string& cgroup);

  // This function is invoked when the polling on eventfd has a
  // result.
  void oomWaited(
      const ContainerID& containerId,
      const std::string& cgroup,
      const process::Future<Nothing>& future);

  // Start listening on memory pressure events.
  void pressureListen(
      const ContainerID& containerId,
      const std::string& cgroup);

  // Stores cgroups associated information for container.
  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_ISOLATOR_SUBSYSTEMS_MEMORY_HPP__
