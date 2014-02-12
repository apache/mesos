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

#ifndef __TEST_CONTAINERIZER_HPP__
#define __TEST_CONTAINERIZER_HPP__

#include <unistd.h>

#include <gmock/gmock.h>

#include <map>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "mesos/executor.hpp"
#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/slave.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Forward declaration.
class MockExecutor;

class TestContainerizer : public slave::Containerizer
{
public:
  TestContainerizer(const hashmap<ExecutorID, Executor*>& executors);

  TestContainerizer(const ExecutorID& executorId, Executor* executor);

  TestContainerizer(MockExecutor* executor);

  TestContainerizer();

  virtual ~TestContainerizer();

  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<slave::Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<slave::Containerizer::Termination> wait(
      const ContainerID& containerId);

  // Additional destroy method for testing because we won't know the
  // ContainerID created for each container.
  void destroy(const FrameworkID& frameworkId, const ExecutorID& executorId);

  virtual void destroy(const ContainerID& containerId);

  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(const Option<slave::state::SlaveState>&));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(const ContainerID&, const Resources&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(const ContainerID&));

private:
  void setup();

  hashmap<ExecutorID, Executor*> executors;

  hashmap<std::pair<FrameworkID, ExecutorID>, ContainerID> containers;
  hashmap<ContainerID, process::Owned<MesosExecutorDriver> > drivers;
  hashmap<ContainerID,
          process::Owned<process::Promise<slave::Containerizer::Termination> > > promises;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_CONTAINERIZER_HPP__
