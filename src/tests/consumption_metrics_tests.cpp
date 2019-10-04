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

#include <string>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "master/detector/standalone.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/master/mock_master_api_subscriber.hpp"

using mesos::internal::master::Master;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using mesos::internal::tests::v1::MockMasterAPISubscriber;

using process::Clock;
using process::Future;
using process::Owned;

using std::string;

using testing::_;
using testing::Return;
using testing::DoAll;


namespace mesos {
namespace internal {
namespace tests {

// Tests for metrics tracking quota consumption.
//
// TODO(asekretenko): Add more tests:
// - ensure hierarchial tracking
// - ensure that shared resource accounting is correct
// - ...
class ConsumptionMetricsTest : public MesosTest {};

// This test ensures that quota consumption of a launched task
// is tracked correctly in the allocator.
//
// The quota consumption metric is expected:
// - to be absent after the scheduler has subscribed
// - to be still absent after the scheduler receives an offer
// - to have the correct value after task launch
// - to be absent after the task terminates
TEST_F(ConsumptionMetricsTest, Launch)
{
  const string cpuConsumedKey = "allocator/mesos/quota/roles/" +
                                v1::DEFAULT_FRAMEWORK_INFO.roles(0) +
                                "/resources/cpus/consumed";

  const master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  // Create a scheduler and set expectations on it.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Subscribe the framework.
  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // There should be no consumption metric at this point.
  Clock::pause();
  Clock::settle();
  EXPECT_NONE(Metrics().at<JSON::Number>(cpuConsumedKey));

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  // There should still be no consumption at this point,
  // as the offer has not been accepted yet.
  Clock::settle();
  EXPECT_NONE(Metrics().at<JSON::Number>(cpuConsumedKey));

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Acknowledge task status updates.
  // We will also need to wait for TASK_RIUNNING and TASK_KILLED.
  const auto sendAcknowledge =
    v1::scheduler::SendAcknowledge(frameworkId, agentId);

  EXPECT_CALL(*scheduler, update(_, _))
    .WillRepeatedly(sendAcknowledge);

  Future<Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .WillOnce(DoAll(FutureArg<1>(&taskRunning), sendAcknowledge))
    .WillRepeatedly(sendAcknowledge);

  Future<Event::Update> taskKilled;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_KILLED)))
    .WillOnce(DoAll(FutureArg<1>(&taskKilled), sendAcknowledge))
    .WillRepeatedly(sendAcknowledge);

  // Launch a task.
  const TaskInfo task = createTask(devolve(offer), SLEEP_COMMAND(100000));
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({evolve(task)})}));

  AWAIT_READY(taskRunning);
  Clock::settle();

  // Allocator should now report the resources as quota consumption.
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);

  // Kill the task and wait for update.
  mesos.send(v1::createCallKill(frameworkId, evolve(task.task_id()), agentId));
  AWAIT_READY(taskKilled);

  // There should be no consumption metric after the terminal task
  // status update has been processed by the master and the allocator.
  Clock::settle();
  EXPECT_NONE(Metrics().at<JSON::Number>(cpuConsumedKey));
}


// This test ensures that reservation is counted as consuming role's quota
// regardless of whether it has been allocated or not.
TEST_F(ConsumptionMetricsTest, ReservedResource)
{
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus(role1):2;mem(role1):1024";

  const string cpuConsumedKey = "allocator/mesos/quota/roles/" +
                                frameworkInfo.roles(0) +
                                "/resources/cpus/consumed";

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  // Create a scheduler and set expectations on it.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Subscribe the framework.
  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // Create a slave with reserved resources.
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  // Test that reservation is consuming role's quota.
  Clock::pause();
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Acknowledge task status updates.
  // We will also need to wait for TASK_RIUNNING and TASK_KILLED.
  const auto sendAcknowledge =
    v1::scheduler::SendAcknowledge(frameworkId, agentId);

  EXPECT_CALL(*scheduler, update(_, _))
    .WillRepeatedly(sendAcknowledge);

  Future<Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .WillOnce(DoAll(FutureArg<1>(&taskRunning), sendAcknowledge))
    .WillRepeatedly(sendAcknowledge);

  Future<Event::Update> taskKilled;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_KILLED)))
    .WillOnce(DoAll(FutureArg<1>(&taskKilled), sendAcknowledge))
    .WillRepeatedly(sendAcknowledge);

  // Launch a task.
  const TaskInfo task = createTask(devolve(offer), SLEEP_COMMAND(100000));
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({evolve(task)})}));

  AWAIT_READY(taskRunning);
  Clock::settle();

  // No double accounting should occur.
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);

  // Kill the task and wait for update.
  mesos.send(v1::createCallKill(frameworkId, evolve(task.task_id()), agentId));
  AWAIT_READY(taskKilled);

  // The quota consumption should remain the same.
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);
}


// This test ensures that quota consumption in allocator
// is tracked correctly during and after master failover.
//
// The quota consumption metric is expected to have the correct value:
// - after task launch
// - after agent re-registration on failover before the framework resubscribes
// - after the framework re-subscribes too
TEST_F(ConsumptionMetricsTest, MasterFailover)
{
  const string cpuConsumedKey = "allocator/mesos/quota/roles/" +
                                v1::DEFAULT_FRAMEWORK_INFO.roles(0) +
                                "/resources/cpus/consumed";

  const master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  // Create a scheduler and set expectations on it.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> reconnected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO))
    .WillOnce(FutureSatisfy(&reconnected));

  Future<Event::Subscribed> subscribed;
  Future<Nothing> resubscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed))
    .WillOnce(FutureSatisfy(&resubscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  // Subscribe the framework.
  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Acknowledge task status updates.
  // We will also need to wait for TASK_RUNNING.
  const auto sendAcknowledge =
    v1::scheduler::SendAcknowledge(frameworkId, agentId);

  EXPECT_CALL(*scheduler, update(_, _))
    .WillRepeatedly(sendAcknowledge);

  Future<Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .WillOnce(DoAll(FutureArg<1>(&taskRunning), sendAcknowledge))
    .WillRepeatedly(sendAcknowledge);

  // Launch a task.
  const TaskInfo task = createTask(devolve(offer), SLEEP_COMMAND(100000));
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({evolve(task)})}));

  AWAIT_READY(taskRunning);

  // If due to some bug the resources are not reported as quota consumption,
  // it makes no sense to continue.
  Clock::pause();
  Clock::settle();
  ASSERT_EQ(2, Metrics().values[cpuConsumedKey]);

  // Failover the master
  Clock::resume();
  detector.appoint(None());
  master->reset();
  AWAIT_READY(disconnected);

  master = StartMaster();
  ASSERT_SOME(master);

  // Subscribe to master API to wait for the agent to re-register.
  MockMasterAPISubscriber masterSubscriber;
  Future<Nothing> agentReAdded;
  EXPECT_CALL(masterSubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentReAdded));

  AWAIT_READY(masterSubscriber.subscribe(master.get()->pid));

  // Have the agent reregister with the new master.
  detector.appoint(master.get()->pid);
  AWAIT_READY(agentReAdded);

  // After agent re-registers, allocator should still report the resources
  // as quota consumption despite the fact that the framework is disconnected.
  Clock::pause();
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);

  // Now we can proceed with reconnecting.
  AWAIT_READY(reconnected);

  // Resubscribe the framework.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  *frameworkInfo.mutable_id() = frameworkId;
  mesos.send(v1::createCallSubscribe(frameworkInfo, frameworkId));
  AWAIT_READY(resubscribed);

  // Ensure that no double accounting occurs after re-subscription.
  Clock::settle();
  EXPECT_EQ(2, Metrics().values[cpuConsumedKey]);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
