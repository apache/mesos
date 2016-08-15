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

#ifndef __TEST_CONTAINERIZER_HPP__
#define __TEST_CONTAINERIZER_HPP__

#include <unistd.h>

#include <map>
#include <memory>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/v1/executor.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "slave/containerizer/containerizer.hpp"

#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/mesos.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Forward declaration.
class MockExecutor;

class TestContainerizer : public slave::Containerizer
{
public:
  TestContainerizer(
      const ExecutorID& executorId,
      const std::shared_ptr<MockV1HTTPExecutor>& executor);

  TestContainerizer(const hashmap<ExecutorID, Executor*>& executors);

  TestContainerizer(const ExecutorID& executorId, Executor* executor);

  explicit TestContainerizer(MockExecutor* executor);

  TestContainerizer();

  virtual ~TestContainerizer();

  virtual process::Future<hashset<ContainerID>> containers();

  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(const Option<slave::state::SlaveState>&));

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
      process::Future<Nothing>(const ContainerID&, const Resources&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(const ContainerID&));

  MOCK_METHOD1(
      status,
      process::Future<ContainerStatus>(const ContainerID&));

  MOCK_METHOD1(
      wait,
      process::Future<containerizer::Termination>(const ContainerID&));

  MOCK_METHOD1(
      destroy,
      void(const ContainerID&));

  // Additional destroy method for testing because we won't know the
  // ContainerID created for each container.
  void destroy(const FrameworkID& frameworkId, const ExecutorID& executorId);

private:
  void setup();

  // Default implementations of mock methods.
  process::Future<bool> _launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const std::map<std::string, std::string>& environment,
      bool checkpoint);

  process::Future<containerizer::Termination> _wait(
      const ContainerID& containerId);

  void _destroy(const ContainerID& containerID);

  hashmap<ExecutorID, Executor*> executors;
  hashmap<ExecutorID, std::shared_ptr<MockV1HTTPExecutor>> v1Executors;

  hashmap<std::pair<FrameworkID, ExecutorID>, ContainerID> containers_;
  hashmap<ContainerID, process::Owned<MesosExecutorDriver>> drivers;
  hashmap<ContainerID, process::Owned<executor::TestV1Mesos>> v1Libraries;
  hashmap<ContainerID,
      process::Owned<process::Promise<containerizer::Termination>>> promises;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_CONTAINERIZER_HPP__
