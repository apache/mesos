/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __DOCKER_CONTAINERIZER_HPP__
#define __DOCKER_CONTAINERIZER_HPP__

#include <process/shared.hpp>

#include <stout/hashset.hpp>

#include "docker/docker.hpp"

#include "slave/containerizer/containerizer.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Prefix used to name Docker containers in order to distinguish those
// created by Mesos from those created manually.
extern std::string DOCKER_NAME_PREFIX;


// Forward declaration.
class DockerContainerizerProcess;


class DockerContainerizer : public Containerizer
{
public:
  static Try<DockerContainerizer*> create(const Flags& flags);

  // This is only public for tests.
  DockerContainerizer(
      const Flags& flags,
      process::Shared<Docker> docker);

  virtual ~DockerContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID> > containers();

private:
  DockerContainerizerProcess* process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_CONTAINERIZER_HPP__
