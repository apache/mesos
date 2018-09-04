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

  Owned<MockResourceProvider> resourceProvider(
      new MockResourceProvider(
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

  resourceProvider->start(endpointDetector, contentType);

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
      {RESERVE(reservedResources, operationId.value())}));

  AWAIT_READY(applyOperationMessage);

  scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_PENDING, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
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

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_RECOVERING, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
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

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_UNKNOWN, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
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

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_UNREACHABLE, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
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

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_GONE_BY_OPERATOR, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
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

  const Future<scheduler::APIResult> result =
    mesos.call({createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const scheduler::Response response = result->response();
  ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const OperationStatus& operationStatus = reconcile.operation_statuses(0);
  EXPECT_EQ(operationId, operationStatus.operation_id());
  EXPECT_EQ(OPERATION_UNKNOWN, operationStatus.state());
  EXPECT_FALSE(operationStatus.has_uuid());
}


// This test verifies that, after a master failover, reconciliation of an
// operation that is still pending on an agent results in `OPERATION_PENDING`.
TEST_P(OperationReconciliationTest, AgentPendingOperationAfterMasterFailover)
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

  Resource disk =
    createDiskResource("200", "*", None(), None(), createDiskSourceRaw());

  Owned<MockResourceProvider> resourceProvider(
      new MockResourceProvider(
          resourceProviderInfo,
          Resources(disk)));

  // We override the mock resource provider's default action, so the operation
  // will stay in `OPERATION_PENDING`.
  Future<resource_provider::Event::ApplyOperation> applyOperation;
  EXPECT_CALL(*resourceProvider, applyOperation(_))
    .WillOnce(FutureArg<0>(&applyOperation));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = GetParam();

  resourceProvider->start(endpointDetector, contentType);

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
          operationId.value())}));

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

  // Test explicit reconciliation
  {
    scheduler::Call::ReconcileOperations::Operation operation;
    operation.mutable_operation_id()->CopyFrom(operationId);
    operation.mutable_agent_id()->CopyFrom(agentId);

    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {operation})});

    AWAIT_READY(result);

    // The master should respond with '200 OK' and with a `scheduler::Response`.
    ASSERT_EQ(process::http::Status::OK, result->status_code());
    ASSERT_TRUE(result->has_response());

    const scheduler::Response response = result->response();
    ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
    ASSERT_TRUE(response.has_reconcile_operations());

    const scheduler::Response::ReconcileOperations& reconcile =
      response.reconcile_operations();
    ASSERT_EQ(1, reconcile.operation_statuses_size());

    const OperationStatus& operationStatus = reconcile.operation_statuses(0);
    EXPECT_EQ(operationId, operationStatus.operation_id());
    EXPECT_EQ(OPERATION_PENDING, operationStatus.state());
    EXPECT_FALSE(operationStatus.has_uuid());
  }

  // Test implicit reconciliation
  {
    const Future<scheduler::APIResult> result =
      mesos.call({createCallReconcileOperations(frameworkId, {})});

    AWAIT_READY(result);

    // The master should respond with '200 OK' and with a `scheduler::Response`.
    ASSERT_EQ(process::http::Status::OK, result->status_code());
    ASSERT_TRUE(result->has_response());

    const scheduler::Response response = result->response();
    ASSERT_EQ(scheduler::Response::RECONCILE_OPERATIONS, response.type());
    ASSERT_TRUE(response.has_reconcile_operations());

    const scheduler::Response::ReconcileOperations& reconcile =
      response.reconcile_operations();
    ASSERT_EQ(1, reconcile.operation_statuses_size());

    const OperationStatus& operationStatus = reconcile.operation_statuses(0);
    EXPECT_EQ(operationId, operationStatus.operation_id());
    EXPECT_EQ(OPERATION_PENDING, operationStatus.state());
    EXPECT_FALSE(operationStatus.has_uuid());
  }
}

} // namespace v1 {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
