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

#ifndef __TESTS_ISOLATOR_HPP__
#define __TESTS_ISOLATOR_HPP__

#include <map>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "slave/isolator.hpp"

namespace mesos {
namespace internal {
namespace tests {

class TestingIsolator : public slave::Isolator
{
public:
  TestingIsolator()
  {
    setup();
  }

  TestingIsolator(const std::map<ExecutorID, Executor*>& _executors)
    : executors(_executors)
  {
    setup();
  }

  TestingIsolator(const ExecutorID& executorId, Executor* executor)
  {
    executors[executorId] = executor;
    setup();
  }

  virtual ~TestingIsolator() {}

  virtual void initialize(
      const slave::Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<slave::Slave>& _slave)
  {
    slave = _slave;
  }

  virtual void launchExecutor(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const UUID& uuid,
      const std::string& directory,
      const Resources& resources,
      const Option<std::string>& path)
  {
    if (executors.count(executorInfo.executor_id()) > 0) {
      Executor* executor = executors[executorInfo.executor_id()];
      MesosExecutorDriver* driver = new MesosExecutorDriver(executor);
      drivers[executorInfo.executor_id()] = driver;

      directories[executorInfo.executor_id()] = directory;

      os::setenv("MESOS_LOCAL", "1");
      os::setenv("MESOS_DIRECTORY", directory);
      os::setenv("MESOS_SLAVE_PID", slave);
      os::setenv("MESOS_SLAVE_ID", slaveId.value());
      os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
      os::setenv("MESOS_EXECUTOR_ID", executorInfo.executor_id().value());
      os::setenv("MESOS_CHECKPOINT", frameworkInfo.checkpoint() ? "1" : "0");

      driver->start();

      os::unsetenv("MESOS_LOCAL");
      os::unsetenv("MESOS_DIRECTORY");
      os::unsetenv("MESOS_SLAVE_PID");
      os::unsetenv("MESOS_SLAVE_ID");
      os::unsetenv("MESOS_FRAMEWORK_ID");
      os::unsetenv("MESOS_EXECUTOR_ID");
      os::unsetenv("MESOS_CHECKPOINT");

      process::dispatch(
          slave,
          &slave::Slave::executorStarted,
          frameworkId,
          executorInfo.executor_id(),
          -1);

    } else {
      FAIL() << "Cannot launch executor";
    }
  }

  virtual void killExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId)
  {
    if (drivers.count(executorId) > 0) {
      MesosExecutorDriver* driver = drivers[executorId];
      driver->stop();
      driver->join();
      delete driver;
      drivers.erase(executorId);

      process::dispatch(
          slave,
          &slave::Slave::executorTerminated,
          frameworkId,
          executorId,
          0,
          false,
          "Killed executor");
    } else {
      FAIL() << "Cannot kill executor";
    }
  }

  // Mocked so tests can check that the resources reflect all started tasks.
  MOCK_METHOD3(resourcesChanged, void(const FrameworkID&,
                                      const ExecutorID&,
                                      const Resources&));

  MOCK_METHOD2(
      usage,
      process::Future<ResourceStatistics>(
          const FrameworkID&,
          const ExecutorID&));

  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(const Option<slave::state::SlaveState>&));

  std::map<ExecutorID, std::string> directories;

private:
  // Helper to setup default expectations.
  void setup()
  {
    EXPECT_CALL(*this, resourcesChanged(testing::_, testing::_, testing::_))
      .Times(testing::AnyNumber());

    EXPECT_CALL(*this, usage(testing::_, testing::_))
      .WillRepeatedly(testing::Return(ResourceStatistics()));

    EXPECT_CALL(*this, recover(testing::_))
      .WillRepeatedly(testing::Return(Nothing()));
  }

  std::map<ExecutorID, Executor*> executors;
  std::map<ExecutorID, MesosExecutorDriver*> drivers;
  process::PID<slave::Slave> slave;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_ISOLATOR_HPP__
