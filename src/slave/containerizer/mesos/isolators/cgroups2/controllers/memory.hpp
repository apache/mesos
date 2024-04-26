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

#ifndef __MEMORY_HPP__
#define __MEMORY_HPP__

#include <string>

#include <process/future.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controller.hpp"

namespace mesos {
namespace internal {
namespace slave {

class MemoryControllerProcess : public ControllerProcess
{
public:
  static Try<process::Owned<ControllerProcess>> create(
      const Flags& flags);

  ~MemoryControllerProcess() override = default;

  std::string name() const override;

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
    process::Future<Nothing> oom;

    process::Promise<mesos::slave::ContainerLimitation> limitation;

    // Check if the hard memory limit has been updated for the container.
    // Also true if the container was recovered.
    bool hardLimitUpdated = false;
  };

  MemoryControllerProcess(const Flags& flags);

  void oomListen(
      const ContainerID& containerId,
      const std::string& cgroup);

  void oomed(
      const ContainerID& containerId,
      const std::string& cgroup,
      const process::Future<Nothing>& oomFuture);

  hashmap<ContainerID, Info> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MEMORY_HPP__