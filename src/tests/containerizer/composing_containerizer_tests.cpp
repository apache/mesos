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

#include <map>
#include <string>
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

using std::map;
using std::string;
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
// launched). If the launch is not supported by the 1st containerizer,
// the composing containerizer should stop the launch loop and
// set the value of destroy future to true.
TEST_F(ComposingContainerizerTest, DestroyDuringUnsupportedLaunchLoop)
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
  map<string, string> environment;

  Promise<bool> launchPromise;

  EXPECT_CALL(*mockContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(Return(launchPromise.future()));

  Future<Nothing> destroy;
  Promise<bool> destroyPromise;
  EXPECT_CALL(*mockContainerizer1, destroy(_))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(destroyPromise.future())));

  Future<bool> launched = containerizer.launch(
      containerId,
      taskInfo,
      executorInfo,
      "dir",
      "user",
      slaveId,
      environment,
      false);

  Resources resources = Resources::parse("cpus:1;mem:256").get();

  EXPECT_TRUE(launched.isPending());

  Future<bool> destroyed = containerizer.destroy(containerId);

  EXPECT_CALL(*mockContainerizer2, launch(_, _, _, _, _, _, _, _))
    .Times(0);

  // We make sure the destroy is being called on the first containerizer.
  // The second containerizer shouldn't be called as well since the
  // container is already destroyed.
  AWAIT_READY(destroy);

  launchPromise.set(false);
  destroyPromise.set(false);

  // `launched` should be a failure and `destroyed` should be true
  // because the launch was stopped from being tried on the 2nd
  // containerizer because of the destroy.
  AWAIT_FAILED(launched);
  AWAIT_EXPECT_EQ(true, destroyed);
}


// This test ensures that destroy can be called while in the
// launch loop. The composing containerizer still calls the
// underlying containerizer's destroy (because it's not sure
// if the containerizer can handle the type of container being
// launched). If the launch is successful the destroy future
// value depends on the containerizer's destroy.
TEST_F(ComposingContainerizerTest, DestroyDuringSupportedLaunchLoop)
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
  map<string, string> environment;

  Promise<bool> launchPromise;

  EXPECT_CALL(*mockContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(Return(launchPromise.future()));

  Future<Nothing> destroy;
  Promise<bool> destroyPromise;
  EXPECT_CALL(*mockContainerizer1, destroy(_))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(destroyPromise.future())));

  Future<bool> launched = containerizer.launch(
      containerId,
      taskInfo,
      executorInfo,
      "dir",
      "user",
      slaveId,
      environment,
      false);

  Resources resources = Resources::parse("cpus:1;mem:256").get();

  EXPECT_TRUE(launched.isPending());

  Future<bool> destroyed = containerizer.destroy(containerId);

  EXPECT_CALL(*mockContainerizer2, launch(_, _, _, _, _, _, _, _))
    .Times(0);

  // We make sure the destroy is being called on the first containerizer.
  // The second containerizer shouldn't be called as well since the
  // container is already destroyed.
  AWAIT_READY(destroy);

  launchPromise.set(true);
  destroyPromise.set(false);

  // `launched` should return true and `destroyed` should return false
  // because the launch succeeded and `destroyPromise` was set to false.
  AWAIT_EXPECT_EQ(true, launched);
  AWAIT_EXPECT_EQ(false, destroyed);
}


// This test ensures that destroy can be called at the end of the
// launch loop. The composing containerizer still calls the
// underlying containerizer's destroy (because it's not sure
// if the containerizer can handle the type of container being
// launched). If the launch is not supported by any containerizers
// both the launch and destroy futures should be false.
TEST_F(ComposingContainerizerTest, DestroyAfterLaunchLoop)
{
  vector<Containerizer*> containerizers;

  MockContainerizer* mockContainerizer1 = new MockContainerizer();
  containerizers.push_back(mockContainerizer1);

  ComposingContainerizer containerizer(containerizers);
  ContainerID containerId;
  containerId.set_value("container");
  TaskInfo taskInfo;
  ExecutorInfo executorInfo;
  SlaveID slaveId;
  map<string, string> environment;

  Promise<bool> launchPromise;

  EXPECT_CALL(*mockContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(Return(launchPromise.future()));

  Future<Nothing> destroy;
  Promise<bool> destroyPromise;
  EXPECT_CALL(*mockContainerizer1, destroy(_))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(destroyPromise.future())));

  Future<bool> launched = containerizer.launch(
      containerId,
      taskInfo,
      executorInfo,
      "dir",
      "user",
      slaveId,
      environment,
      false);

  Resources resources = Resources::parse("cpus:1;mem:256").get();

  EXPECT_TRUE(launched.isPending());

  Future<bool> destroyed = containerizer.destroy(containerId);

  // We make sure the destroy is being called on the containerizer.
  AWAIT_READY(destroy);

  launchPromise.set(false);
  destroyPromise.set(false);

  // `launch` should return false and `destroyed` should return false
  // because none of the containerizers support the launch.
  AWAIT_EXPECT_EQ(false, launched);
  AWAIT_EXPECT_EQ(false, destroyed);
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
