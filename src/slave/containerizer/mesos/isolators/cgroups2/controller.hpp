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

#ifndef __CONTROLLER_HPP__
#define __CONTROLLER_HPP__

#include <mesos/slave/isolator.hpp>

#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class ControllerProcess;

// Abstraction for a cgroups v2 controller.
class Controller
{
public:
  explicit Controller(process::Owned<ControllerProcess> process);

  // Copied from: src/slave/containerizer/mesos/isolators/cgroups/subsystem.hpp
  // We have unique ownership of the wrapped process and
  // enforce that objects of this class cannot be copied.
  //
  // TODO(bbannier): Remove this once MESOS-5122 is resolved.
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  ~Controller();

  // Name of the controller.
  std::string name() const;

  process::Future<Nothing> recover(
      const ContainerID& containerId,
      const std::string& cgroup);

  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const std::string& cgroup,
      const mesos::slave::ContainerConfig& containerConfig);

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      const std::string& cgroup,
      pid_t pid);

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId,
      const std::string& cgroup);

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const std::string& cgroup,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {});

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId,
      const std::string& cgroup);

  process::Future<ContainerStatus> status(
      const ContainerID& containerId,
      const std::string& cgroup);

  process::Future<Nothing> cleanup(
      const ContainerID& containerId,
      const std::string& cgroup);

private:
  process::Owned<ControllerProcess> process;
};


class ControllerProcess : public process::Process<ControllerProcess>
{
public:
  ~ControllerProcess() override {}

  virtual std::string name() const = 0;

  virtual process::Future<Nothing> recover(
      const ContainerID& containerId,
      const std::string& cgroup);

  virtual process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const std::string& cgroup,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      const std::string& cgroup,
      pid_t pid);

  virtual process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId,
      const std::string& cgroup);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const std::string& cgroup,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {});

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId,
      const std::string& cgroup);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId,
      const std::string& cgroup);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId,
      const std::string& cgroup);

protected:
  ControllerProcess(const Flags& flags);

  // Flags used to launch the agent.
  const Flags flags;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CONTROLLER_HPP__
