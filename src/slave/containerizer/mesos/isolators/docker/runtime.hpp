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

#ifndef __DOCKER_RUNTIME_ISOLATOR_HPP__
#define __DOCKER_RUNTIME_ISOLATOR_HPP__

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The docker runtime isolator is responsible for preparing mesos
// container by merging runtime configuration specified by user
// and docker image default configuration.
class DockerRuntimeIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~DockerRuntimeIsolatorProcess() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

private:
  DockerRuntimeIsolatorProcess(const Flags& flags);

  Option<Environment> getLaunchEnvironment(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  Result<CommandInfo> getLaunchCommand(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  Option<std::string> getWorkingDirectory(
      const mesos::slave::ContainerConfig& containerConfig);

  Option<std::string> getContainerUser(
      const mesos::slave::ContainerConfig& containerConfig);

  const Flags flags;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_RUNTIME_ISOLATOR_HPP__
