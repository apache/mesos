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

#ifndef __TESTS_MOCKDOCKER_HPP__
#define __TESTS_MOCKDOCKER_HPP__

#include <list>
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <mesos/slave/container_logger.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/docker.hpp"

#include "slave/containerizer/mesos/isolators/gpu/components.hpp"

using ::testing::_;
using ::testing::Invoke;

using mesos::internal::slave::NvidiaComponents;

namespace mesos {
namespace internal {
namespace tests {

// Definition of a mock Docker to be used in tests with gmock.
class MockDocker : public Docker
{
public:
  MockDocker(
      const std::string& path,
      const std::string& socket,
      const Option<JSON::Object>& config = None());
  virtual ~MockDocker();

  MOCK_CONST_METHOD3(
      run,
      process::Future<Option<int>>(
          const Docker::RunOptions& options,
          const process::Subprocess::IO&,
          const process::Subprocess::IO&));

  MOCK_CONST_METHOD2(
      ps,
      process::Future<std::list<Docker::Container>>(
          bool, const Option<std::string>&));

  MOCK_CONST_METHOD3(
      pull,
      process::Future<Docker::Image>(
          const std::string&,
          const std::string&,
          bool));

  MOCK_CONST_METHOD3(
      stop,
      process::Future<Nothing>(
          const std::string&,
          const Duration&,
          bool));

  MOCK_CONST_METHOD2(
      inspect,
      process::Future<Docker::Container>(
          const std::string&,
          const Option<Duration>&));

  process::Future<Option<int>> _run(
      const Docker::RunOptions& runOptions,
      const process::Subprocess::IO& _stdout,
      const process::Subprocess::IO& _stderr) const
  {
    return Docker::run(
        runOptions,
        _stdout,
        _stderr);
  }

  process::Future<std::list<Docker::Container>> _ps(
      bool all,
      const Option<std::string>& prefix) const
  {
    return Docker::ps(all, prefix);
  }

  process::Future<Docker::Image> _pull(
      const std::string& directory,
      const std::string& image,
      bool force) const
  {
    return Docker::pull(directory, image, force);
  }

  process::Future<Nothing> _stop(
      const std::string& containerName,
      const Duration& timeout,
      bool remove) const
  {
    return Docker::stop(containerName, timeout, remove);
  }

  process::Future<Docker::Container> _inspect(
      const std::string& containerName,
      const Option<Duration>& retryInterval)
  {
    return Docker::inspect(containerName, retryInterval);
  }
};


// Definition of a mock DockerContainerizer to be used in tests with gmock.
class MockDockerContainerizer : public slave::DockerContainerizer {
public:
  MockDockerContainerizer(
      const slave::Flags& flags,
      slave::Fetcher* fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& logger,
      process::Shared<Docker> docker,
      const Option<NvidiaComponents>& nvidia = None());

  MockDockerContainerizer(
      const process::Owned<slave::DockerContainerizerProcess>& process);

  virtual ~MockDockerContainerizer();

  void initialize()
  {
    // NOTE: See TestContainerizer::setup for why we use
    // 'EXPECT_CALL' and 'WillRepeatedly' here instead of
    // 'ON_CALL' and 'WillByDefault'.
    EXPECT_CALL(*this, launch(_, _, _, _, _, _, _, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizer::_launch));

    EXPECT_CALL(*this, update(_, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizer::_update));
  }

  MOCK_METHOD8(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const Option<TaskInfo>&,
          const ExecutorInfo&,
          const std::string&,
          const Option<std::string>&,
          const SlaveID&,
          const std::map<std::string, std::string>&,
          bool checkpoint));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(
          const ContainerID&,
          const Resources&));

  // Default 'launch' implementation (necessary because we can't just
  // use &slave::DockerContainerizer::launch with 'Invoke').
  process::Future<bool> _launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const std::map<std::string, std::string>& environment,
      bool checkpoint)
  {
    return slave::DockerContainerizer::launch(
        containerId,
        taskInfo,
        executorInfo,
        directory,
        user,
        slaveId,
        environment,
        checkpoint);
  }

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources)
  {
    return slave::DockerContainerizer::update(
        containerId,
        resources);
  }
};


// Definition of a mock DockerContainerizerProcess to be used in tests
// with gmock.
class MockDockerContainerizerProcess : public slave::DockerContainerizerProcess
{
public:
  MockDockerContainerizerProcess(
      const slave::Flags& flags,
      slave::Fetcher* fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& logger,
      const process::Shared<Docker>& docker,
      const Option<NvidiaComponents>& nvidia = None());

  virtual ~MockDockerContainerizerProcess();

  MOCK_METHOD2(
      fetch,
      process::Future<Nothing>(
          const ContainerID& containerId,
          const SlaveID& slaveId));

  MOCK_METHOD1(
      pull,
      process::Future<Nothing>(const ContainerID& containerId));

  process::Future<Nothing> _fetch(
      const ContainerID& containerId,
      const SlaveID& slaveId)
  {
    return slave::DockerContainerizerProcess::fetch(containerId, slaveId);
  }

  process::Future<Nothing> _pull(const ContainerID& containerId)
  {
    return slave::DockerContainerizerProcess::pull(containerId);
  }
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCKDOCKER_HPP__
