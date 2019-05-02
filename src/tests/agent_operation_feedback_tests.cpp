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

#include <mesos/mesos.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;

using process::http::OK;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {
namespace v1 {

class AgentOperationFeedbackTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    AgentOperationFeedbackTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));

// This test verifies that operation status updates are resent to the master
// until they are acknowledged.
//
// To accomplish this:
//   1. Reserves agent default resources.
//   2. Expects the framework to receive an operation status update.
//   3. Advances the clock and verifies that the agent resends the operation
//      status update.
//   4. Makes the framework acknowledge the operation status update.
//   5. Advances the clock and verifies that the agent doesn't resend the
//      operation status update.
TEST_P(AgentOperationFeedbackTest, RetryOperationStatusUpdate)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);

  const FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);

  // Reserve resources.
  OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, update->status().state());

  // The master has already seen the update, so the operation is counted
  // in the metrics.
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // The agent should resend the unacknowledged operation status update once the
  // status update retry interval lapses.
  Future<scheduler::Event::UpdateOperationStatus> retriedUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&retriedUpdate));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(retriedUpdate);

  EXPECT_EQ(operationId, retriedUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, retriedUpdate->status().state());

  // The scheduler will acknowledge the operation status update, so the agent
  // should receive an acknowledgement.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
        AcknowledgeOperationStatusMessage(),
        master.get()->pid,
        slave.get()->pid);

  mesos.send(createCallAcknowledgeOperationStatus(
      frameworkId, offer.agent_id(), None(), retriedUpdate.get()));

  AWAIT_READY(acknowledgeOperationStatusMessage);

  // Verify that the update has not been double-counted.
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // The operation status update has been acknowledged, so the agent shouldn't
  // send further status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MAX);
  Clock::settle();
  Clock::resume();
}

// This test verifies that operations affecting agent default resources are
// removed from the master in-memory state once a terminal status update is
// acknowledged.
TEST_P(AgentOperationFeedbackTest, CleanupAcknowledgedTerminalOperation)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  const FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);

  // Reserve resources.
  OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, update->status().state());
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  {
    master::Call call;
    call.set_type(master::Call::GET_OPERATIONS);

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::PROTOBUF);

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(ContentType::PROTOBUF, call),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<master::Response> response_ =
      deserialize<master::Response>(ContentType::PROTOBUF, response->body);

    ASSERT_SOME(response_);
    const master::Response::GetOperations& operations =
      response_->get_operations();

    ASSERT_EQ(1, operations.operations_size());
    EXPECT_EQ(
        OPERATION_FINISHED,
        operations.operations(0).latest_status().state());
    EXPECT_EQ(operationId, operations.operations(0).info().id());
  }

  // The scheduler will acknowledge the operation status update, so the agent
  // should receive an acknowledgement.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
        AcknowledgeOperationStatusMessage(),
        master.get()->pid,
        slave.get()->pid);

  mesos.send(createCallAcknowledgeOperationStatus(
      frameworkId, offer.agent_id(), None(), update.get()));

  AWAIT_READY(acknowledgeOperationStatusMessage);

  // The master should no longer track the operation.
  {
    master::Call call;
    call.set_type(master::Call::GET_OPERATIONS);

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::PROTOBUF);

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(ContentType::PROTOBUF, call),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<master::Response> response_ =
      deserialize<master::Response>(ContentType::PROTOBUF, response->body);

    ASSERT_SOME(response_);
    const master::Response::GetOperations& operations =
      response_->get_operations();

    ASSERT_EQ(0, operations.operations_size());
  }
}

// This test verifies that operation status updates for operations affecting
// agent default resources include the agent ID of the originating agent.
TEST_P(AgentOperationFeedbackTest, OperationUpdateContainsAgentID)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ContentType contentType = GetParam();

  scheduler::TestMesos mesos(master.get()->pid, contentType, scheduler);

  AWAIT_READY(subscribed);

  const FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);

  const AgentID& agentId(offer.agent_id());

  // Reserve resources.
  OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, update->status().state());
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  ASSERT_TRUE(update->status().has_agent_id());
  EXPECT_EQ(agentId, update->status().agent_id());

  {
    master::Call call;
    call.set_type(master::Call::GET_OPERATIONS);

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<master::Response> response_ =
      deserialize<master::Response>(contentType, response->body);

    ASSERT_SOME(response_);
    const master::Response::GetOperations& operations =
      response_->get_operations();

    ASSERT_EQ(1, operations.operations_size());
    EXPECT_EQ(
        OPERATION_FINISHED,
        operations.operations(0).latest_status().state());
    EXPECT_EQ(operationId, operations.operations(0).info().id());

    ASSERT_TRUE(operations.operations(0).latest_status().has_agent_id());
    EXPECT_EQ(agentId, operations.operations(0).latest_status().agent_id());
  }
}


// When a scheduler requests feedback for an operation and the operation is
// dropped en route to the agent, the scheduler should receive an
// OPERATION_DROPPED status update.
TEST_P(AgentOperationFeedbackTest, DroppedOperationStatusUpdate)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);

  const FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);

  // Reserve resources.
  OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  // Drop the operation in its way to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(applyOperationMessage);

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Resume the clock to avoid deadlocks related to agent registration.
  // See MESOS-8828.
  Clock::resume();

  // Restart the agent to trigger operation reconciliation. This is reasonable
  // because dropped messages from master to agent should only occur when there
  // is an agent disconnection.
  slave->reset();

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlaveMessage);

  Clock::pause();

  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(OPERATION_DROPPED, update->status().state());
  EXPECT_FALSE(update->status().has_uuid());
  EXPECT_FALSE(update->status().has_resource_provider_id());
  EXPECT_TRUE(metricEquals("master/operations/dropped", 1));

  const AgentID& agentId(offer.agent_id());
  ASSERT_TRUE(update->status().has_agent_id());
  EXPECT_EQ(agentId, update->status().agent_id());

  Clock::resume();
}


// When a scheduler requests feedback for an operation and the operation is
// dropped en route to the agent, it's possible that a `ReregisterSlaveMessage`
// from the agent races with a `SlaveReregisteredMessage` from the master. In
// this case, master/agent reconciliation of the operation may be performed more
// than once, leading to duplicate operation status updates. This test verifies
// that the master gracefully handles such a sequence of events. This is a
// regression test for MESOS-9698.
TEST_P(AgentOperationFeedbackTest, DroppedOperationDuplicateStatusUpdate)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);

  const FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);

  // Reserve resources.
  OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  // Drop the operation in its way to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(applyOperationMessage);

  // Capture the update on the way from agent to master
  // so that we can inject a duplicate.
  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Resume the clock to avoid deadlocks related to agent registration.
  // See MESOS-8828.
  Clock::resume();

  // Restart the agent to trigger operation reconciliation. This is reasonable
  // because dropped messages from master to agent should only occur when there
  // is an agent disconnection.
  slave->reset();

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlaveMessage);

  Clock::pause();

  AWAIT_READY(updateOperationStatusMessage);
  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(OPERATION_DROPPED, update->status().state());
  EXPECT_FALSE(update->status().has_uuid());
  EXPECT_FALSE(update->status().has_resource_provider_id());
  EXPECT_TRUE(metricEquals("master/operations/dropped", 1));

  const AgentID& agentId(offer.agent_id());
  ASSERT_TRUE(update->status().has_agent_id());
  EXPECT_EQ(agentId, update->status().agent_id());

  // Inject a duplicate operation status update. Before resolution of
  // MESOS-9698, this would crash the master.
  process::post(
      slave.get()->pid,
      master.get()->pid,
      updateOperationStatusMessage.get());

  // Since a terminal update was already received by the master, nothing should
  // be forwarded to the scheduler now. Settle the clock to ensure that we would
  // notice if this happens.
  Clock::settle();

  Clock::resume();
}

} // namespace v1 {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
