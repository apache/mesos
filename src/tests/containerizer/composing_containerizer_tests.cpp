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

#include <vector>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/gmock.hpp>

#include <stout/option.hpp>

#include "messages/messages.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/composing.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::slave;

using namespace process;

using std::vector;

using testing::_;
using testing::Return;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {


class ComposingContainerizerTest : public MesosTest {};

class MockContainerizer : public slave::Containerizer
{
public:
  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(
          const Option<slave::state::SlaveState>&));

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
          bool));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(
          const ContainerID&,
          const Resources&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(
          const ContainerID&));

  MOCK_METHOD1(
      wait,
      process::Future<ContainerTermination>(
          const ContainerID&));

  MOCK_METHOD1(
      destroy,
      void(const ContainerID&));

  MOCK_METHOD0(
      containers,
      process::Future<hashset<ContainerID>>());
};


// This test checks if destroy is called while container is being
// launched, the composing containerizer still calls the underlying
// containerizer's destroy and skip calling the rest of the
// containerizers.
TEST_F(ComposingContainerizerTest, DestroyWhileLaunching)
{
  vector<Containerizer*> containerizers;

  MockContainerizer* mockContainerizer = new MockContainerizer();
  MockContainerizer* mockContainerizer2 = new MockContainerizer();

  containerizers.push_back(mockContainerizer);
  containerizers.push_back(mockContainerizer2);

  ComposingContainerizer containerizer(containerizers);
  ContainerID containerId;
  containerId.set_value("container");
  TaskInfo taskInfo;
  ExecutorInfo executorInfo;
  SlaveID slaveId;
  std::map<std::string, std::string> environment;

  Promise<bool> launchPromise;

  EXPECT_CALL(*mockContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(Return(launchPromise.future()));

  Future<Nothing> destroy;

  EXPECT_CALL(*mockContainerizer, destroy(_))
    .WillOnce(FutureSatisfy(&destroy));

  Future<bool> launch = containerizer.launch(
      containerId,
      taskInfo,
      executorInfo,
      "dir",
      "user",
      slaveId,
      environment,
      false);

  Resources resources = Resources::parse("cpus:1;mem:256").get();

  EXPECT_TRUE(launch.isPending());

  containerizer.destroy(containerId);

  EXPECT_CALL(*mockContainerizer2, launch(_, _, _, _, _, _, _, _))
    .Times(0);

  // We make sure the destroy is being called on the first containerizer.
  // The second containerizer shouldn't be called as well since the
  // container is already destroyed.
  AWAIT_READY(destroy);

  launchPromise.set(false);
  AWAIT_FAILED(launch);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
