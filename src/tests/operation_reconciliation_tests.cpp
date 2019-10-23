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

#include <memory>
#include <utility>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/v1/mesos.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/message.hpp>

#include <stout/gtest.hpp>

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;

using testing::Eq;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {
namespace v1 {

class OperationReconciliationTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    OperationReconciliationTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// This test ensures that the master responds with `OPERATION_PENDING` for
// operations that are pending at the master.
TEST_P(OperationReconciliationTest, PendingOperation)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.

  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Resource disk =
    createDiskResource("200", "*", None(), None(), createDiskSourceRaw());

  Owned<TestResourceProvider> resourceProvider(
      new TestResourceProvider(
          resourceProviderInfo,
          Resources(disk)));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = GetParam();

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  Clock::pause();

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(scheduler::DeclineOffers()); // Decline subsequent offers.

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);
  const AgentID& agentId = offer.agent_id();

  // We'll drop the `ApplyOperationMessage` from the master to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), master.get()->pid, _);

  Resources resources =
    Resources(offer.resources()).filter([](const Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  Resource reservedResources = *(resources.begin());
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  OperationID operationId;
  operationId.set_value("operation");

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(applyOperationMessage);

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_PENDING, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test ensures that the master responds to a reconciliation request with
// `OPERATION_PENDING` and `OPERATION_FINISHED` when there are unacked operation
// updates with those statuses stored in the master.
TEST_P(OperationReconciliationTest, PendingAndFinishedOperations)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<scheduler::Event::Offers> offers;

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(scheduler::DeclineOffers()); // Decline subsequent offers.

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);
  const AgentID& agentId = offer.agent_id();

  // Note that the below XXXX_PROTOBUF calls are evaluated in the reverse order
  // in which they are declared, and this code relies on the operations being
  // forwarded to the agent in the same order in which they are submitted by the
  // scheduler.

  // We'll capture the second `ApplyOperationMessage` sent
  // from the master to the agent.
  Future<ApplyOperationMessage> allowedApplyOperationMessage =
    FUTURE_PROTOBUF(ApplyOperationMessage(), master.get()->pid, _);

  // We'll drop the first `ApplyOperationMessage` sent
  // from the master to the agent.
  Future<ApplyOperationMessage> droppedApplyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), master.get()->pid, _);

  Try<Resources> cpuResources = Resources::parse("cpus:0.01");
  ASSERT_SOME(cpuResources);

  Resource reservedCpu = *(cpuResources->begin());
  reservedCpu.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  OperationID operationId1;
  operationId1.set_value("operation1");

  OperationID operationId2;
  operationId2.set_value("operation2");

  Future<scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedCpu, operationId1),
       RESERVE(reservedCpu, operationId2)}));

  AWAIT_READY(droppedApplyOperationMessage);
  AWAIT_READY(allowedApplyOperationMessage);

  AWAIT_READY(update);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate1;
  EXPECT_CALL(
      *scheduler,
      updateOperationStatus(_, scheduler::OperationStatusUpdateOperationIdEq(
          operationId1)))
    .WillOnce(FutureArg<1>(&reconciliationUpdate1));

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate2;
  EXPECT_CALL(
      *scheduler,
      updateOperationStatus(_, scheduler::OperationStatusUpdateOperationIdEq(
          operationId2)))
    .WillOnce(FutureArg<1>(&reconciliationUpdate2));

  scheduler::Call::ReconcileOperations::Operation operation1;
  operation1.mutable_operation_id()->CopyFrom(operationId1);
  operation1.mutable_agent_id()->CopyFrom(agentId);

  scheduler::Call::ReconcileOperations::Operation operation2;
  operation2.mutable_operation_id()->CopyFrom(operationId2);
  operation2.mutable_agent_id()->CopyFrom(agentId);

  const Future<scheduler::APIResult> result = mesos.call(
      createCallReconcileOperations(frameworkId, {operation1, operation2}));

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate1);
  AWAIT_READY(reconciliationUpdate2);

  EXPECT_EQ(operationId1, reconciliationUpdate1->status().operation_id());
  EXPECT_EQ(OPERATION_PENDING, reconciliationUpdate1->status().state());
  EXPECT_FALSE(reconciliationUpdate1->status().has_uuid());

  EXPECT_EQ(operationId2, reconciliationUpdate2->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate2->status().state());
  EXPECT_FALSE(reconciliationUpdate2->status().has_uuid());
}


// This test verifies that reconciliation of an unknown operation that belongs
// to an agent that has been recovered from the registry after master failover
// but has not yet registered, results in `OPERATION_RECOVERING`.
//
// TODO(gkleiman): Enable this test on Windows once Windows supports the
// replicated log.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    OperationReconciliationTest, UnknownOperationRecoveredAgent)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register and get the agent ID.
  AWAIT_READY(slaveRegisteredMessage);
  const AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

  // Stop the master.
  master->reset();

  // Stop the slave.
  slave.get()->terminate();
  slave->reset();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline all offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  OperationID operationId;
  operationId.set_value("operation");

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_RECOVERING, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that reconciliation of an unknown operation that belongs
// to a known agent results in `OPERATION_UNKNOWN`.
TEST_P(OperationReconciliationTest, UnknownOperationKnownAgent)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register and get the agent ID.
  AWAIT_READY(slaveRegisteredMessage);
  const AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline all offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  OperationID operationId;
  operationId.set_value("operation");

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_UNKNOWN, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that reconciliation of an unknown operation that belongs
// to an unreachable agent results in `OPERATION_UNREACHABLE`.
TEST_P(OperationReconciliationTest, UnknownOperationUnreachableAgent)
{
  Clock::pause();

  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Allow the master to PING the agent, but drop all PONG messages
  // from the agent. Note that we don't match on the master / agent
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping =
    FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register and get the agent ID.
  AWAIT_READY(slaveRegisteredMessage);
  const AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

  // Now, induce a partition of the agent by having the master
  // timeout the agent.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);
  Clock::settle();

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline all offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  OperationID operationId;
  operationId.set_value("operation");

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_UNREACHABLE, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that reconciliation of an unknown operation that belongs
// to an agent marked gone results in `OPERATION_GONE_BY_OPERATOR`.
TEST_P(OperationReconciliationTest, UnknownOperationAgentMarkedGone)
{
  Clock::pause();

  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register and get the agent ID.
  AWAIT_READY(slaveRegisteredMessage);
  const AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

  ContentType contentType = GetParam();

  {
    master::Call call;
    call.set_type(master::Call::MARK_AGENT_GONE);

    call.mutable_mark_agent_gone()->mutable_agent_id()->CopyFrom(agentId);

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  }

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline all offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  scheduler::TestMesos mesos(master.get()->pid, contentType, scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  OperationID operationId;
  operationId.set_value("operation");

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_GONE_BY_OPERATOR, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that reconciliation of an unknown operation that belongs
// to an unknown agent results in `OPERATION_UNKNOWN`.
TEST_P(OperationReconciliationTest, UnknownOperationUnknownAgent)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline all offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  AgentID agentId;
  agentId.set_value("agent");

  OperationID operationId;
  operationId.set_value("operation");

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_UNKNOWN, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that, after a master failover, reconciliation of an
// operation that is still pending on an agent results in `OPERATION_PENDING`.
TEST_P(
    OperationReconciliationTest,
    DISABLED_AgentPendingOperationAfterMasterFailover)
{
  Clock::pause();

  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.

  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Resource disk = createDiskResource(
      "200", "*", None(), None(), createDiskSourceRaw(None(), "profile"));

  Owned<TestResourceProvider> resourceProvider(
      new TestResourceProvider(
          resourceProviderInfo,
          Resources(disk)));

  // We override the mock resource provider's default action, so the operation
  // will stay in `OPERATION_PENDING`.
  Future<resource_provider::Event::ApplyOperation> applyOperation;
  EXPECT_CALL(*resourceProvider->process, applyOperation(_))
    .WillOnce(FutureArg<0>(&applyOperation));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = GetParam();

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  Clock::pause();

  // Start a v1 framework.
  auto scheduler = std::make_shared<MockHTTPScheduler>();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  // Decline offers that do not contain wanted resources.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(scheduler::DeclineOffers());

  Future<scheduler::Event::Offers> offers;

  auto isRaw = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().type() == Resource::DiskInfo::Source::RAW;
  };

  EXPECT_CALL(*scheduler, offers(_, scheduler::OffersHaveAnyResource(
      std::bind(isRaw, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(scheduler::DeclineOffers()); // Decline successive offers.

  scheduler::TestMesos mesos(
      master.get()->pid, contentType, scheduler, detector);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  // NOTE: If the framework has not declined an unwanted offer yet when
  // the master updates the agent with the RAW disk resource, the new
  // allocation triggered by this update won't generate an allocatable
  // offer due to no CPU and memory resources. So here we first settle
  // the clock to ensure that the unwanted offer has been declined, then
  // advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);
  const AgentID& agentId = offer.agent_id();

  Option<Resource> source;
  Option<ResourceProviderID> resourceProviderId;
  foreach (const Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      source = resource;

      ASSERT_TRUE(resource.has_provider_id());
      resourceProviderId = resource.provider_id();

      break;
    }
  }

  ASSERT_SOME(source);
  ASSERT_SOME(resourceProviderId);

  OperationID operationId;
  operationId.set_value("operation");

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {CREATE_DISK(
           source.get(),
           Resource::DiskInfo::Source::MOUNT,
           None(),
           operationId)}));

  AWAIT_READY(applyOperation);

  // Simulate master failover.
  EXPECT_CALL(*scheduler, disconnected(_));

  detector->appoint(None());

  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes recovering the registry.
  Clock::settle();

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo, frameworkId));

  Future<scheduler::Event::Subscribed> frameworkResubscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&frameworkResubscribed));

  // Simulate a new master detected event to the agent and the scheduler.
  detector->appoint(master.get()->pid);

  // Advance the clock, so that the agent re-registers.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Resume the clock to avoid deadlocks related to agent registration.
  // See MESOS-8828.
  Clock::resume();

  // Wait for the framework and agent to re-register.
  AWAIT_READY(slaveReregistered);
  AWAIT_READY(updateSlaveMessage);
  AWAIT_READY(frameworkResubscribed);

  Clock::pause();

  // Test explicit reconciliation.
  {
    scheduler::Call::ReconcileOperations::Operation operation;
    operation.mutable_operation_id()->CopyFrom(operationId);
    operation.mutable_agent_id()->CopyFrom(agentId);

    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {operation})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_PENDING, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }

  // Test implicit reconciliation.
  {
    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_PENDING, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }
}

// This test ensures that the master responds with the latest state for
// operations affecting agent default resources that are terminal at the
// master, but have not been acknowledged by the framework.
TEST_P(OperationReconciliationTest, ReconcileUnackedAgentOperation)
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

  scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

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

  // Test explicit reconciliation.
  {
    scheduler::Call::ReconcileOperations::Operation operation;
    operation.mutable_operation_id()->CopyFrom(operationId);
    operation.mutable_agent_id()->CopyFrom(agentId);

    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {operation})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }

  // Test implicit reconciliation.
  {
    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }
}

// This test verifies that after a master failover a reconciliation request for
// an operation affecting agent default resources with a pending unacknowledged
// status update results in `OPERATION_FINISHED`.
TEST_P(OperationReconciliationTest, UnackedAgentOperationAfterMasterFailover)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(slaveRegisteredMessage);

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
      GetParam(),
      scheduler,
      detector);

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

  // Simulate master failover.
  EXPECT_CALL(*scheduler, disconnected(_));

  detector->appoint(None());

  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes recovering the registry.
  Clock::settle();

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo, frameworkId));

  Future<scheduler::Event::Subscribed> frameworkResubscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&frameworkResubscribed));

  // After agent reregistration, the operation status update manager is resumed,
  // so the operation update will be resent.
  Future<scheduler::Event::UpdateOperationStatus> resentUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&resentUpdate));

  // Simulate a new master detected event to the agent and the scheduler.
  detector->appoint(master.get()->pid);

  // Advance the clock, so that the agent re-registers.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Resume the clock to avoid deadlocks related to agent registration.
  // See MESOS-8828.
  Clock::resume();

  // Wait for the framework and agent to re-register.
  AWAIT_READY(slaveReregistered);
  AWAIT_READY(updateSlaveMessage);
  AWAIT_READY(frameworkResubscribed);
  AWAIT_READY(resentUpdate);

  Clock::pause();

  // Test explicit reconciliation.
  {
    scheduler::Call::ReconcileOperations::Operation operation;
    operation.mutable_operation_id()->CopyFrom(operationId);
    operation.mutable_agent_id()->CopyFrom(agentId);

    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {operation})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }

  // Test implicit reconciliation.
  {
    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }
}

// This test ensures that when an agent is marked as gone, the master sends
// OPERATION_GONE_BY_OPERATOR updates for operations affecting agent default
// resources that have unacknowledged status updates, and that it responds with
// the same state to reconciliation requests.
TEST_P(OperationReconciliationTest, ReconcileAgentOperationOnGoneAgent)
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

  scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

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

  Future<scheduler::Event::UpdateOperationStatus> goneUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&goneUpdate));

  // Ignore `FAILURE` event triggered by the `MARK_AGENT_GONE` operation.
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(Return());

  {
    master::Call call;
    call.set_type(master::Call::MARK_AGENT_GONE);

    call.mutable_mark_agent_gone()->mutable_agent_id()->CopyFrom(agentId);

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  }

  AWAIT_READY(goneUpdate);

  EXPECT_EQ(operationId, goneUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_GONE_BY_OPERATOR, goneUpdate->status().state());

  // TODO(bevers): We have to reset the agent before we can access the
  // metrics to work around MESOS-9644.
  slave->reset();
  EXPECT_TRUE(metricEquals("master/operations/gone_by_operator", 1));

  ASSERT_TRUE(goneUpdate->status().has_agent_id());
  EXPECT_EQ(agentId, goneUpdate->status().agent_id());

  // Test explicit reconciliation.
  {
    scheduler::Call::ReconcileOperations::Operation operation;
    operation.mutable_operation_id()->CopyFrom(operationId);
    operation.mutable_agent_id()->CopyFrom(agentId);

    Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .WillOnce(FutureArg<1>(&reconciliationUpdate));

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {operation})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    AWAIT_READY(reconciliationUpdate);

    EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
    EXPECT_EQ(
        OPERATION_GONE_BY_OPERATOR,
        reconciliationUpdate->status().state());
    EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
  }

  // Test implicit reconciliation.
  //
  // NOTE: The master removes from its in-memory state operations from agents
  // marked gone, so no operation status update should be sent in response to
  // the reconciliation request.
  {
    EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
      .Times(0);

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {})});

    AWAIT_READY(result);

    // The master should respond with '202 Accepted' and an
    // empty body (response field unset).
    ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
    EXPECT_FALSE(result->has_response());

    // Settle the clock to ensure that no further operation status updates are
    // received by the scheduler.
    Clock::settle();
  }
}


// This test ensures that the master responds with `OPERATION_RECOVERING` for an
// operation on resources offered by a resource provider which is not currently
// subscribed with a registered agent.
TEST_P(OperationReconciliationTest, OperationOnUnsubscribedProvider)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.
  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Resource disk =
    createDiskResource("200", "*", None(), None(), createDiskSourceRaw());

  Owned<TestResourceProvider> resourceProvider(
      new TestResourceProvider(
          resourceProviderInfo,
          Resources(disk)));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = GetParam();

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  Clock::pause();

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(scheduler::DeclineOffers()); // Decline subsequent offers.

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  scheduler::TestMesos mesos(master.get()->pid, GetParam(), scheduler);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);
  const AgentID& agentId = offer.agent_id();

  // We'll drop the `ApplyOperationMessage` from the master to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), master.get()->pid, _);

  Resources resources =
    Resources(offer.resources()).filter([](const Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  Resource reservedResources = *(resources.begin());
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  ResourceProviderID resourceProviderId(reservedResources.provider_id());

  OperationID operationId;
  operationId.set_value("operation");

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(applyOperationMessage);

  // Tear the resource provider down.
  resourceProvider.reset();

  // Make sure the resource provider manager processes the disconnection.
  Clock::settle();

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);
  operation.mutable_resource_provider_id()->CopyFrom(resourceProviderId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_RECOVERING, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// Verifies that when the master forwards a framework-initiated reconciliation
// request to the agent, and this forwarded request races with an
// `UpdateSlaveMessage` which includes information about a resource provider
// included in that reconciliation request, the agent will correctly fulfill the
// reconciliation request by sending an operation status update containing the
// latest state of the operation to the framework.
TEST_P(
    OperationReconciliationTest,
    FrameworkReconciliationRaceWithUpdateSlaveMessage)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.
  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Resource disk =
    createDiskResource("200", "*", None(), None(), createDiskSourceRaw());

  Owned<TestResourceProvider> resourceProvider(
      new TestResourceProvider(
          resourceProviderInfo,
          Resources(disk)));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = GetParam();

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources. At this point the resource provider
  // will have an ID assigned by the agent.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  Future<v1::ResourceProviderID> resourceProviderId =
    resourceProvider->process->id();

  AWAIT_READY(resourceProviderId);

  resourceProviderInfo.mutable_id()->CopyFrom(resourceProviderId.get());

  Clock::pause();

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(scheduler::SendSubscribe(frameworkInfo));

  Future<scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(scheduler::DeclineOffers()); // Decline subsequent offers.

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  scheduler::TestMesos mesos(
      master.get()->pid,
      GetParam(),
      scheduler,
      detector);

  AWAIT_READY(subscribed);
  FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const Offer& offer = offers->offers(0);
  const AgentID& agentId = offer.agent_id();

  Resources resources =
    Resources(offer.resources()).filter([](const Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  Resource reservedResources = *(resources.begin());
  reservedResources.add_reservations()->CopyFrom(
      createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  OperationID operationId;
  operationId.set_value("operation");

  // Don't acknowledge the terminal operation status update so that the
  // operation remains in the resource provider and agent state.
  Future<v1::scheduler::Event::UpdateOperationStatus> finishedUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&finishedUpdate));

  mesos.send(createCallAccept(
      frameworkId,
      offer,
      {RESERVE(reservedResources, operationId)}));

  AWAIT_READY(finishedUpdate);
  EXPECT_EQ(operationId, finishedUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, finishedUpdate->status().state());
  EXPECT_TRUE(finishedUpdate->status().has_uuid());

  Future<mesos::v1::scheduler::Mesos*> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .Times(1)
    .WillOnce(FutureArg<0>(&disconnected));

  Future<mesos::v1::scheduler::Mesos*> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .Times(1)
    .WillOnce(FutureArg<0>(&connected));

  // Drop the `UpdateSlaveMessage` which contains the resubscribed resource
  // provider in order to simulate a race between this message and the
  // RECONCILE_OPERATIONS scheduler call below.
  updateSlaveMessage = DROP_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Restart the master to clear out the master's operation state and force
  // an agent reregistration. Also appoint `None()` to avoid a spurious
  // detection of the master since it reuses the same `UPID`.
  detector->appoint(None());
  master->reset();
  AWAIT_READY(disconnected);

  master = StartMaster();
  ASSERT_SOME(master);

  detector->appoint(master.get()->pid);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to reregister.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  Future<scheduler::Event::Subscribed> resubscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&resubscribed));

  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  AWAIT_READY(connected);
  mesos.send(createCallSubscribe(frameworkInfo, frameworkId));

  AWAIT_READY(resubscribed);

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);
  operation.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  // Verify that the master forwards the reconciliation request to the agent.
  Future<ReconcileOperationsMessage> reconcileOperationsMessage =
    FUTURE_PROTOBUF(ReconcileOperationsMessage(), _, _);

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(reconcileOperationsMessage);
  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  EXPECT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(OPERATION_FINISHED, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}

} // namespace v1 {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
