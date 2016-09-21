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
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/composing.hpp"

#include "tests/mesos.hpp"

#include "tests/containerizer/mock_containerizer.hpp"

using namespace mesos::internal::slave;

using namespace process;

using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

class ComposingContainerizerTest : public MesosTest {};


// This test ensures that destroy can be called while in the
// launch loop. The composing containerizer still calls the
// underlying containerizer's destroy (because it's not sure
// if the containerizer can handle the type of container being
// launched). Once the launch is rejected, the composing
// containerizer should stop the launch loop.
TEST_F(ComposingContainerizerTest, DestroyDuringLaunchLoop)
{
  vector<Containerizer*> containerizers;

  MockContainerizer* mockContainerizer1 = new MockContainerizer();
  MockContainerizer* mockContainerizer2 = new MockContainerizer();

  containerizers.push_back(mockContainerizer1);
  containerizers.push_back(mockContainerizer2);

  ComposingContainerizer containerizer(containerizers);
  ContainerID containerId;
  containerId.set_value("container");
  TaskInfo taskInfo;
  ExecutorInfo executorInfo;
  SlaveID slaveId;
  std::map<std::string, std::string> environment;

  Promise<bool> launchPromise;

  EXPECT_CALL(*mockContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(Return(launchPromise.future()));

  Future<Nothing> destroy;

  EXPECT_CALL(*mockContainerizer1, destroy(_))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(Future<bool>(false))));

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


// Ensures the containerizer responds correctly (false Future) to
// a request to destroy an unknown container.
TEST_F(ComposingContainerizerTest, DestroyUnknownContainer)
{
  vector<Containerizer*> containerizers;

  MockContainerizer* mockContainerizer1 = new MockContainerizer();
  MockContainerizer* mockContainerizer2 = new MockContainerizer();

  containerizers.push_back(mockContainerizer1);
  containerizers.push_back(mockContainerizer2);

  ComposingContainerizer containerizer(containerizers);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  AWAIT_EXPECT_FALSE(containerizer.destroy(containerId));
}


// Ensures the containerizer responds correctly (returns None)
// to a request to wait on an unknown container.
TEST_F(ComposingContainerizerTest, WaitUnknownContainer)
{
  vector<Containerizer*> containerizers;

  MockContainerizer* mockContainerizer1 = new MockContainerizer();
  MockContainerizer* mockContainerizer2 = new MockContainerizer();

  containerizers.push_back(mockContainerizer1);
  containerizers.push_back(mockContainerizer2);

  ComposingContainerizer containerizer(containerizers);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Future<Option<ContainerTermination>> wait = containerizer.wait(containerId);

  AWAIT_READY(wait);
  EXPECT_NONE(wait.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
