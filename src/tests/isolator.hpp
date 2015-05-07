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

#ifndef __TEST_ISOLATOR_HPP__
#define __TEST_ISOLATOR_HPP__

#include <gmock/gmock.h>

#include <mesos/slave/isolator.hpp>

namespace mesos {
namespace internal {
namespace tests {

class TestIsolatorProcess : public mesos::slave::IsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Option<CommandInfo>& commandInfo)
  {
    process::Owned<mesos::slave::IsolatorProcess> process(
        new TestIsolatorProcess(commandInfo));

    return new mesos::slave::Isolator(process);
  }

  MOCK_METHOD2(
      recover,
      process::Future<Nothing>(
          const std::list<mesos::slave::ExecutorRunState>&,
          const hashset<ContainerID>&));

  virtual process::Future<Option<CommandInfo>> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& rootfs,
      const Option<std::string>& user)
  {
    return commandInfo;
  }

  MOCK_METHOD2(
      isolate,
      process::Future<Nothing>(const ContainerID&, pid_t));

  MOCK_METHOD1(
      watch,
      process::Future<mesos::slave::Limitation>(const ContainerID&));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(const ContainerID&, const Resources&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(const ContainerID&));

  MOCK_METHOD1(
      cleanup,
      process::Future<Nothing>(const ContainerID&));

private:
  TestIsolatorProcess(const Option<CommandInfo>& _commandInfo)
    : commandInfo(_commandInfo)
  {
    EXPECT_CALL(*this, watch(testing::_))
      .WillRepeatedly(testing::Return(promise.future()));

    EXPECT_CALL(*this, isolate(testing::_, testing::_))
      .WillRepeatedly(testing::Return(Nothing()));

    EXPECT_CALL(*this, cleanup(testing::_))
      .WillRepeatedly(testing::Return(Nothing()));
  }


  const Option<CommandInfo> commandInfo;

  process::Promise<mesos::slave::Limitation> promise;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_ISOLATOR_HPP__
