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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/owned.hpp>

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Mesos;

using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

// Tests that exercise the default executor implementation
// should be located in this file.

class DefaultExecutorTest : public MesosTest {};

// This test verifies that the default executor can launch a task group.
TEST_F(DefaultExecutorTest, TaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockV1HTTPScheduler>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::TestV1Mesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  Future<v1::scheduler::Event::Update> update1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update1);

  ASSERT_EQ(TASK_RUNNING, update1->status().state());
  EXPECT_EQ(taskInfo.task_id(), update1->status().task_id());

  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update2));

  {
    // Acknowledge TASK_RUNNING update.
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(update2);

  ASSERT_EQ(TASK_FINISHED, update2->status().state());
  EXPECT_EQ(taskInfo.task_id(), update2->status().task_id());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
