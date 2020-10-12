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
#include <string>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/lambda.hpp>
#include <stout/try.hpp>

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/allocator/mesos/allocator.hpp"
#include "master/detector/standalone.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/master/mock_master_api_subscriber.hpp"

using mesos::internal::master::Master;
using mesos::internal::master::allocator::MesosAllocatorProcess;
using mesos::internal::recordio::Reader;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::v1::FrameworkInfo;
using mesos::v1::scheduler::APIResult;
using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Promise;

using recordio::Decoder;

using google::protobuf::RepeatedPtrField;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;


namespace mesos {
namespace internal {
namespace tests {


template <class TFrameworkInfo>
static TFrameworkInfo changeAllMutableFields(const TFrameworkInfo& oldInfo)
{
  CHECK_EQ(TFrameworkInfo::descriptor()->field_count(), 13)
    << "After adding a new mutable field to FrameworkInfo, please make sure "
    << "that this function modifies this field";

  TFrameworkInfo newInfo = oldInfo;

  *newInfo.mutable_name() += "_foo";
  newInfo.set_failover_timeout(newInfo.failover_timeout() + 1000.0);
  *newInfo.mutable_hostname() += ".foo";
  *newInfo.mutable_webui_url() += "/foo";

  newInfo.add_capabilities()->set_type(
      TFrameworkInfo::Capability::REGION_AWARE);

  auto* newLabel = newInfo.mutable_labels()->add_labels();
  *newLabel->mutable_key() = "UPDATE_FRAMEWORK_KEY";
  *newLabel->mutable_value() = "UPDATE_FRAMEWORK_VALUE";

  // TODO(asekretenko): Test update of `role` with a non-MULTI_ROLE framework.
  newInfo.add_roles("new_role");

  CHECK(newInfo.offer_filters().count("new_role") == 0);
  (*newInfo.mutable_offer_filters())["new_role"] =
    typename std::remove_reference<decltype(
      newInfo.offer_filters())>::type::mapped_type();

  return newInfo;
}

namespace v1 {


static Future<v1::master::Response::GetFrameworks> getFrameworks(
    const process::PID<Master>& pid)
{
  v1::master::Call call;
  call.set_type(v1::master::Call::GET_FRAMEWORKS);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(ContentType::PROTOBUF);

  return process::http::post(
           pid,
           "api/v1",
           headers,
           serialize(ContentType::PROTOBUF, call),
           stringify(ContentType::PROTOBUF))
    .then([](const process::http::Response& httpResponse)
        -> Future<v1::master::Response::GetFrameworks> {
        if (httpResponse.status != process::http::OK().status) {
          return Failure(
            "GET_FRAMEWORKS failed with response status " +
            httpResponse.status);
        }

        Try<v1::master::Response> response =
          deserialize<mesos::v1::master::Response>(
            ContentType::PROTOBUF, httpResponse.body);

        if (response.isError()) {
          return Failure(response.error());
        }

        if (!response->has_get_frameworks()) {
          return Failure("Response to GET_FRAMEWORKS has no 'get_frameworks'");
        }

        return response->get_frameworks();
      });
}


namespace scheduler {


class UpdateFrameworkTest : public MesosTest {};


static Future<APIResult> callUpdateFramework(
    Mesos* mesos,
    const FrameworkInfo& info,
    const vector<string>& suppressedRoles = {},
    const Option<::mesos::v1::scheduler::OfferConstraints>& offerConstraints =
      None())
{
  CHECK(info.has_id());

  Call call;
  call.set_type(Call::UPDATE_FRAMEWORK);
  *call.mutable_framework_id() = info.id();
  *call.mutable_update_framework()->mutable_framework_info() = info;
  *call.mutable_update_framework()->mutable_suppressed_roles() =
    RepeatedPtrField<string>(suppressedRoles.begin(), suppressedRoles.end());

  if (offerConstraints.isSome()) {
    *call.mutable_update_framework()->mutable_offer_constraints() =
      *offerConstraints;
  }

  return mesos->call(call);
}


TEST_F(UpdateFrameworkTest, UserChangeFails)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  *update.mutable_id() = subscribed->framework_id();
  *update.mutable_user() += "_foo";

  Future<APIResult> result = callUpdateFramework(&mesos, update);

  AWAIT_READY(result);
  EXPECT_EQ(result->status_code(), 400u);
  EXPECT_TRUE(strings::contains(
      result->error(), "Updating 'FrameworkInfo.user' is unsupported"));

  // Check that no partial update occurred.
  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);
  AWAIT_READY(frameworks);
  ASSERT_EQ(frameworks->frameworks_size(), 1);

  FrameworkInfo expected = DEFAULT_FRAMEWORK_INFO;
  *expected.mutable_id() = subscribed->framework_id();
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(::mesos::v1::typeutils::diff(update, expected));
}


TEST_F(UpdateFrameworkTest, PrincipalChangeFails)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  *update.mutable_id() = subscribed->framework_id();
  *update.mutable_principal() += "_foo";

  Future<APIResult> result = callUpdateFramework(&mesos, update);

  AWAIT_READY(result);
  EXPECT_EQ(result->status_code(), 400u);
  EXPECT_TRUE(strings::contains(
      result->error(), "Changing framework's principal is not allowed"));

  // Check that no partial update occurred.
  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);
  AWAIT_READY(frameworks);

  ASSERT_EQ(frameworks->frameworks_size(), 1);

  FrameworkInfo expected = DEFAULT_FRAMEWORK_INFO;
  *expected.mutable_id() = subscribed->framework_id();
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(::mesos::v1::typeutils::diff(update, expected));
}


TEST_F(UpdateFrameworkTest, CheckpointingChangeFails)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  *update.mutable_id() = subscribed->framework_id();
  update.set_checkpoint(!update.checkpoint());

  Future<APIResult> result = callUpdateFramework(&mesos, update);

  AWAIT_READY(result);
  EXPECT_EQ(result->status_code(), 400u);
  EXPECT_TRUE(strings::contains(
      result->error(), "Updating 'FrameworkInfo.checkpoint' is unsupported"));

  // Check that no partial update occurred.
  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);

  AWAIT_READY(frameworks);

  ASSERT_EQ(frameworks->frameworks_size(), 1);

  FrameworkInfo expected = DEFAULT_FRAMEWORK_INFO;
  *expected.mutable_id() = subscribed->framework_id();
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(::mesos::v1::typeutils::diff(update, expected));
}


// TODO(asekretenko): Add more tests for invalid updates:
// - Try to add a nonexisting role.
// - Try to add a role which the framework is not authorized to use.
// ....


// This test checks that it is possible to update all the mutable fields.
// It verifies the following:
// - HTTP status code of the scheduler API response.
// - FrameworkInfo returned by GetFrameworks API call.
// - FrameworkInfo sent in UpdateFrameworkMessage to the slave.
// - FrameworkInfo sent in FRAMEWORK_UPDATED to the API subscribers.
TEST_F(UpdateFrameworkTest, MutableFieldsUpdateSuccessfully)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  // Subscribe to master v1 API.
  MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  // We need a slave to test the UpdateFrameworkMessage.
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // To test the UpdateFrameworkMessage, we should wait for the slave
  // to be added before calling UPDATE_FRAMEWORK.
  AWAIT_READY(agentAdded);

  // Expect FRAMEWORK_UPDATED event
  Future<v1::master::Event::FrameworkUpdated> frameworkUpdated;
  EXPECT_CALL(masterAPISubscriber, frameworkUpdated(_))
    .WillOnce(FutureArg<0>(&frameworkUpdated));

  // Expect UpdateFrameworkMessage to be sent from master to slave.
  Future<UpdateFrameworkMessage> updateFrameworkMessage = FUTURE_PROTOBUF(
      UpdateFrameworkMessage(), master->get()->pid, slave->get()->pid);

  // Start scheduler, wait for connection and subscribe
  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  *update.mutable_id() = subscribed->framework_id();

  Future<APIResult> result = callUpdateFramework(&mesos, update);

  AWAIT_READY(result);
  EXPECT_EQ(result->status_code(), 200u);

  // Check that update occurred.
  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);
  AWAIT_READY(frameworks);

  ASSERT_EQ(frameworks->frameworks_size(), 1);
  const FrameworkInfo& frameworkInfo =
    frameworks->frameworks(0).framework_info();

  EXPECT_NONE(::mesos::v1::typeutils::diff(frameworkInfo, update));

  AWAIT_READY(updateFrameworkMessage);
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      evolve(updateFrameworkMessage->framework_info()), update));

  AWAIT_READY(frameworkUpdated);
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworkUpdated->framework().framework_info(), update));
};


// This test issues two UpdateFrameworkCalls: the first one with the same
// `FrameworkInfo`, the second with mutated `FrameworkInfo`,
// and verifies that the first call does NOT result in updates
// to agents/subscribers.
TEST_F(UpdateFrameworkTest, NoRedundantUpdates)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  // Subscribe to master v1 API.
  MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  // We need an agent to test the UpdateFrameworkMessage.
  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // To test the UpdateFrameworkMessage, we should wait for the agent
  // to be added before calling UPDATE_FRAMEWORK.
  AWAIT_READY(agentAdded);

  // Expect a single FRAMEWORK_UPDATED event.
  Future<v1::master::Event::FrameworkUpdated> frameworkUpdated;
  EXPECT_CALL(masterAPISubscriber, frameworkUpdated(_))
    .WillOnce(FutureArg<0>(&frameworkUpdated));

  // Expect UpdateFrameworkMessage to be sent from the master to the agent.
  Future<UpdateFrameworkMessage> updateFrameworkMessage = FUTURE_PROTOBUF(
      UpdateFrameworkMessage(), master->get()->pid, slave->get()->pid);

  // Start the scheduler, wait for connection and then subscribe.
  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  // To send UPDATE_FRAMEWORK, we need to obtain a framework ID.
  AWAIT_READY(subscribed);

  // Issue an UPDATE_FRAMEWORK that does not touch `FrameworkInfo`.
  FrameworkInfo update1 = DEFAULT_FRAMEWORK_INFO;
  *update1.mutable_id() = subscribed->framework_id();
  Future<APIResult> result1 = callUpdateFramework(&mesos, update1);

  AWAIT_READY(result1);
  ASSERT_EQ(result1->status_code(), 200u);

  // Verify that the first update has not resulted in broadcasts to
  // agents/subscribers.
  Clock::pause();
  Clock::settle();
  ASSERT_TRUE(frameworkUpdated.isPending());
  ASSERT_TRUE(updateFrameworkMessage.isPending());

  // Change `FrameworkInfo` via UPDATE_FRAMEWORK.
  const FrameworkInfo update2 = changeAllMutableFields(update1);
  Future<APIResult> result2 = callUpdateFramework(&mesos, update2);

  AWAIT_READY(result2);
  EXPECT_EQ(result2->status_code(), 200u);

  // Verify that the broadcasts report the second update.
  AWAIT_READY(updateFrameworkMessage);
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      evolve(updateFrameworkMessage->framework_info()), update2));

  AWAIT_READY(frameworkUpdated);
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworkUpdated->framework().framework_info(), update2));
};


// This tests that adding a role via UPDATE_FRAMEWORK to a framework which had
// no roles triggers allocation of an offer for that role.
TEST_F(UpdateFrameworkTest, OffersOnAddingRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // There are at least two distinct cases that one might want to test:
  // - That adding a role triggers allocation.
  // - That adding a slave triggers allocation when the framework has roles.
  //
  // In this test the intention is to test the first case - and definitely
  // not to alternate between these two cases from run to run.
  // Therefore, before making scheduler calls, we need to wait for the slave to
  // be added. This is done by waiting for an AGENT_ADDED master API event.
  MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  Owned<MasterDetector> detector = master->get()->createDetector();

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(agentAdded);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  // Initially, the framework subscribes with no roles.
  FrameworkInfo initialFrameworkInfo = DEFAULT_FRAMEWORK_INFO;
  initialFrameworkInfo.clear_roles();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(initialFrameworkInfo));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Expect that the framework gets no offers before update.
  EXPECT_CALL(*scheduler, offers(_, _))
    .Times(AtMost(0));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  // Trigger allocation to ensure that offers are not generated before update.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Expect an offer after adding a role.
  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  FrameworkInfo update = initialFrameworkInfo;
  *update.mutable_id() = subscribed->framework_id();
  update.add_roles("new_role");

  // To spare test running time, we wait for the update response and advance
  // clock after that.
  AWAIT_READY(callUpdateFramework(&mesos, update));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);

  ASSERT_EQ(offers->offers().size(), 1);
  EXPECT_EQ(offers->offers(0).allocation_info().role(), "new_role");
}


// Test that framework's offers are rescinded when a framework is
// removed from all its roles via UPDATE_FRAMEWORK.
TEST_F(UpdateFrameworkTest, RescindOnRemovingRoles)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Expect offers. This should happen exactly once (when both
  // the slave is added and the framework is subscribed).
  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(offers);
  ASSERT_EQ(offers->offers().size(), 1);

  // Set up expectations for things that should happen after role removal.

  // The offer for the removed role should be rescinded.
  Future<Event::Rescind> rescind;
  EXPECT_CALL(*scheduler, rescind(_, _))
    .WillOnce(FutureArg<1>(&rescind));

  // recoverResources() should be called.
  //
  // TODO(asekretenko): Add a more in-depth check that the
  // allocator does what it should.
  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Remove the framework from the role via UPDATE_FRAMEWORK.
  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  *update.mutable_id() = subscribed->framework_id();
  update.clear_roles();

  callUpdateFramework(&mesos, update);

  AWAIT_READY(rescind);
  AWAIT_READY(recoverResources);

  EXPECT_EQ(offers->offers(0).id(), rescind->offer_id());

  // After that, nothing of interest should happen within an allocation
  // interval: no more offers and no more rescinding.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
}


// This test ensures that it is possible to add
// a suppressed role via UPDATE_FRAMEWORK.
TEST_F(UpdateFrameworkTest, AddSuppressedRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  // Initially, the framework subscribes with no roles.
  FrameworkInfo initialFrameworkInfo = DEFAULT_FRAMEWORK_INFO;
  initialFrameworkInfo.clear_roles();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribe(initialFrameworkInfo));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Expect that the framework gets no offers before update.
  EXPECT_CALL(*scheduler, offers(_, _))
    .Times(AtMost(0));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  // Add a suppressed role.
  FrameworkInfo update = initialFrameworkInfo;
  *update.mutable_id() = subscribed->framework_id();
  update.add_roles("new_role");

  AWAIT_READY(callUpdateFramework(&mesos, update, {"new_role"}));

  // Trigger allocation to ensure that offers are not generated.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
}


// Helper action for RemoveAndUnsuppress.
ACTION_P(SendSubscribeWithAllRolesSuppressed, framework)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);
  *call.mutable_subscribe()->mutable_framework_info() = framework;
  *call.mutable_subscribe()->mutable_suppressed_roles() = framework.roles();

  arg0->send(call);
}


// This test ensures that it is possible to remove roles both from the roles
// set and the suppressed roles set.
TEST_F(UpdateFrameworkTest, RemoveAndUnsuppress)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(SendSubscribeWithAllRolesSuppressed(DEFAULT_FRAMEWORK_INFO));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Expect that the framework gets no offers.
  EXPECT_CALL(*scheduler, offers(_, _))
    .Times(AtMost(0));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  // Remove suppressed roles while unsuppressing them.
  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  *update.mutable_id() = subscribed->framework_id();
  update.clear_roles();

  AWAIT_READY(callUpdateFramework(&mesos, update, {}));

  // Trigger allocation to ensure that nothing happens.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
}


// This test ensures that it is possible to modify offer constraints
// via the UpdateFramework call.
TEST_F(UpdateFrameworkTest, OfferConstraints)
{
  using ::mesos::v1::scheduler::AttributeConstraint;
  using ::mesos::v1::scheduler::OfferConstraints;

  const Try<JSON::Object> initialConstraintsJson =
    JSON::parse<JSON::Object>(R"~(
        {
          "role_constraints": {
            ")~" + DEFAULT_FRAMEWORK_INFO.roles(0) + R"~(": {
              "groups": [{
                "attribute_constraints": [{
                  "selector": {"attribute_name": "foo"},
                  "predicate": {"exists": {}}
                }]
              }]
            }
          }
        })~");

  ASSERT_SOME(initialConstraintsJson);

  const Try<OfferConstraints> initialConstraints =
    ::protobuf::parse<OfferConstraints>(*initialConstraintsJson);

  ASSERT_SOME(initialConstraints);

  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(Invoke([initialConstraints](Mesos* mesos) {
      Call call;
      call.set_type(Call::SUBSCRIBE);
      *call.mutable_subscribe()->mutable_framework_info() =
        DEFAULT_FRAMEWORK_INFO;

      *call.mutable_subscribe()->mutable_offer_constraints() =
        *initialConstraints;

      mesos->send(call);
    }));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _)).WillOnce(FutureArg<1>(&subscribed));

  // Expect that the framework gets no offers.
  EXPECT_CALL(*scheduler, offers(_, _)).Times(AtMost(0));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  // Trigger allocation to ensure that the agent is not offered before changing
  // offer constraints.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Expect an offer after constraints change.
  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _)).WillOnce(FutureArg<1>(&offers));

  // Change constraint to `NotExists` so that the agent will now be offered to
  // the framework.
  const Try<JSON::Object> updatedConstraintsJson =
    JSON::parse<JSON::Object>(R"~(
        {
          "role_constraints": {
            ")~" + DEFAULT_FRAMEWORK_INFO.roles(0) + R"~(": {
              "groups": [{
                "attribute_constraints": [{
                  "selector": {"attribute_name": "foo"},
                  "predicate": {"not_exists": {}}
                }]
              }]
            }
          }
        })~");

  ASSERT_SOME(updatedConstraintsJson);

  const Try<OfferConstraints> updatedConstraints =
    ::protobuf::parse<OfferConstraints>(*updatedConstraintsJson);

  ASSERT_SOME(updatedConstraints);

  {
    FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
    *framework.mutable_id() = subscribed->framework_id();

    AWAIT_READY(
        callUpdateFramework(&mesos, framework, {}, *updatedConstraints));
  }

  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  EXPECT_EQ(offers->offers().size(), 1);

  // Ensure that the updated offer constraints are reflected in the master's
  // '/state' response.
  {
    Future<process::http::Response> response = process::http::get(
        master->get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Object> reportedConstraintsJson = parse->find<JSON::Object>(
        "frameworks[0].offer_constraints");

    EXPECT_SOME_EQ(*updatedConstraintsJson, reportedConstraintsJson);
  }

  // Ensure that the updated offer constraints are reflected in the master's
  // '/frameworks' response.
  {
    Future<process::http::Response> response = process::http::get(
        master->get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Object> reportedConstraintsJson = parse->find<JSON::Object>(
        "frameworks[0].offer_constraints");

    ASSERT_SOME_EQ(*updatedConstraintsJson, reportedConstraintsJson);
  }

  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);
  AWAIT_READY(frameworks);

  ASSERT_EQ(frameworks->frameworks_size(), 1);
  ASSERT_TRUE(frameworks->frameworks(0).has_offer_constraints());

  // TODO(asekretenko): As the semantics of the offer constraints does not
  // depend on the order of constraints groups and the order of individual
  // constraints inside groups, we should consider using a `MessageDifferencer`
  // configured to take this into account.
  ASSERT_EQ(
      updatedConstraints->SerializeAsString(),
      frameworks->frameworks(0).offer_constraints().SerializeAsString());
}


// This test ensures that an UPDATE_FRAMEWORK call trying to set offer
// constraints for a role to which the framework will not be subscribed
// fails validation and is rejected as a whole.
TEST_F(UpdateFrameworkTest, OfferConstraintsForUnsubscribedRole)
{
  using ::mesos::v1::scheduler::OfferConstraints;

  const Try<JSON::Object> constraintsJson = JSON::parse<JSON::Object>(
      R"~(
        {
          "role_constraints": {
            ")~" + DEFAULT_FRAMEWORK_INFO.roles(0) + R"~(": {
              "groups": [{
                "attribute_constraints": [{
                  "selector": {"attribute_name": "foo"},
                  "predicate": {"exists": {}}
                }]
              }]
            }
          }
        })~");

  ASSERT_SOME(constraintsJson);

  const Try<OfferConstraints> constraints =
    ::protobuf::parse<OfferConstraints>(*constraintsJson);

  ASSERT_SOME(constraints);

  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(Invoke([constraints](Mesos* mesos) {
      Call call;
      call.set_type(Call::SUBSCRIBE);
      *call.mutable_subscribe()->mutable_framework_info() =
        DEFAULT_FRAMEWORK_INFO;

      *call.mutable_subscribe()->mutable_offer_constraints() = *constraints;

      mesos->send(call);
    }));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Subscribed> subscribed;

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  TestMesos mesos(master->get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  // Try to change framework's role but specify constraints for the old role.
  FrameworkInfo frameworkInfoWithNewRole = DEFAULT_FRAMEWORK_INFO;
  frameworkInfoWithNewRole.clear_roles();
  frameworkInfoWithNewRole.add_roles(DEFAULT_FRAMEWORK_INFO.roles(0) + "_new");
  *frameworkInfoWithNewRole.mutable_id() = subscribed->framework_id();

  Future<APIResult> result =
    callUpdateFramework(&mesos, frameworkInfoWithNewRole, {}, *constraints);

  AWAIT_READY(result);
  EXPECT_EQ(result->status_code(), 400u);
  EXPECT_TRUE(strings::contains(result->error(), "Offer constraints"));

  Future<v1::master::Response::GetFrameworks> frameworks =
    getFrameworks(master->get()->pid);

  AWAIT_READY(frameworks);

  ASSERT_EQ(frameworks->frameworks_size(), 1);

  // The framework info should have remained the same, despite
  // `updatedFrameworkInfo` on its own not containing any invalid updates.
  FrameworkInfo expected = DEFAULT_FRAMEWORK_INFO;
  *expected.mutable_id() = subscribed->framework_id();
  EXPECT_NONE(::mesos::v1::typeutils::diff(
      frameworks->frameworks(0).framework_info(), expected));
}

} // namespace scheduler {
} // namespace v1 {


// Base class for tests of V0 UPDATE_FRAMEWORK call
class UpdateFrameworkV0Test : public MesosTest {};


TEST_F(UpdateFrameworkV0Test, DriverErrorWhenCalledBeforeRegistration)
{
  ::mesos::master::detector::StandaloneMasterDetector detector;

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
    &sched, &detector, DEFAULT_FRAMEWORK_INFO);

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  driver.updateFramework(DEFAULT_FRAMEWORK_INFO, {}, {});

  AWAIT_READY(error);
  EXPECT_EQ(error.get(),
            "MesosSchedulerDriver::updateFramework() must not be called"
            " prior to registration with the master");

  driver.stop();
  driver.join();
}


TEST_F(UpdateFrameworkV0Test, DriverErrorOnFrameworkIDMismatch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
    &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(frameworkId);

  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  *update.mutable_id() = frameworkId.get();
  *update.mutable_id()->mutable_value() += "-deadbeef";

  driver.updateFramework(update, {}, {});

  AWAIT_READY(error);
  EXPECT_EQ(
      error.get(),
      "The 'FrameworkInfo.id' provided to"
      " MesosSchedulerDriver::updateFramework()"
      " (" + stringify(update.id()) + ")"
      " must be equal to the value known to the MesosSchedulerDriver"
      " (" + stringify(frameworkId.get()) + ")");

  driver.stop();
  driver.join();
}


TEST_F(UpdateFrameworkV0Test, CheckpointingChangeFails)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
    &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(frameworkId);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  update.set_checkpoint(!update.checkpoint());
  *update.mutable_id() = frameworkId.get();
  driver.updateFramework(update, {}, {});

  AWAIT_READY(error);
  EXPECT_TRUE(strings::contains(
      error.get(), "Updating 'FrameworkInfo.checkpoint' is unsupported"));

  driver.stop();
  driver.join();
}


TEST_F(UpdateFrameworkV0Test, MutableFieldsUpdateSuccessfully)
{
  Try<Owned<cluster::Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  // Subscribe to master v1 API.
  v1::MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  // We need a slave to test the UpdateFrameworkMessage.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // To test the UpdateFrameworkMessage, we should wait for the slave
  // to be added before calling UPDATE_FRAMEWORK.
  AWAIT_READY(agentAdded);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);

  // Expect FRAMEWORK_UPDATED event after update.
  Future<v1::master::Event::FrameworkUpdated> frameworkUpdated;
  EXPECT_CALL(masterAPISubscriber, frameworkUpdated(_))
    .WillOnce(FutureArg<0>(&frameworkUpdated));

  // Expect UpdateFrameworkMessage to be sent from master to slave.
  Future<UpdateFrameworkMessage> updateFrameworkMessage = FUTURE_PROTOBUF(
      UpdateFrameworkMessage(), master->get()->pid, slave->get()->pid);

  FrameworkInfo update = changeAllMutableFields(DEFAULT_FRAMEWORK_INFO);
  *update.mutable_id() = frameworkId.get();

  driver.updateFramework(update, {}, {});

  AWAIT_READY(updateFrameworkMessage);

  EXPECT_NONE(::mesos::typeutils::diff(
      updateFrameworkMessage->framework_info(), update));

  AWAIT_READY(frameworkUpdated);
  EXPECT_NONE(::mesos::typeutils::diff(
      devolve(frameworkUpdated->framework().framework_info()), update));

  Future<v1::master::Response::GetFrameworks> frameworks =
    v1::getFrameworks(master->get()->pid);
  AWAIT_READY(frameworks);
  ASSERT_EQ(frameworks->frameworks_size(), 1);
  const FrameworkInfo& reportedFrameworkInfo =
    devolve(frameworks->frameworks(0).framework_info());

  EXPECT_NONE(::mesos::typeutils::diff(reportedFrameworkInfo, update));

  driver.stop();
  driver.join();
}


// This tests that adding a role via UPDATE_FRAMEWORK to a framework which had
// no roles triggers allocation of an offer for that role.
TEST_F(UpdateFrameworkV0Test, OffersOnAddingRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // There are at least two distinct cases that one might want to test:
  // - That adding a role triggers allocation.
  // - That adding a slave triggers allocation when the framework has roles.
  //
  // In this test the intention is to test the first case - and definitely
  // not to alternate between these two cases from run to run.
  // Therefore, before making scheduler calls, we need to wait for the slave to
  // be added. This is done by waiting for an AGENT_ADDED master API event.
  v1::MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  Owned<MasterDetector> detector = master->get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(agentAdded);

  // Subscribe without roles.
  FrameworkInfo initialFrameworkInfo = DEFAULT_FRAMEWORK_INFO;
  initialFrameworkInfo.clear_roles();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), initialFrameworkInfo);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Check that the framework gets no offers before update.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(AtMost(0));

  driver.start();

  AWAIT_READY(frameworkId);

  // Trigger allocation to ensure that offers are not generated before update.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
  Clock::resume();

  // Expect an offer after adding a role.
  Future<std::vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  // Add a role via update and wait for offers.
  FrameworkInfo update = initialFrameworkInfo;
  update.clear_roles();
  update.add_roles("new_role");
  *update.mutable_id() = frameworkId.get();

  driver.updateFramework(update, {}, {});

  AWAIT_READY(offers);

  ASSERT_EQ(offers->size(), 1u);
  EXPECT_EQ(offers->front().allocation_info().role(), "new_role");
}

// Test that framework's offers are rescinded when a framework is
// removed from all its roles via UPDATE_FRAMEWORK.
TEST_F(UpdateFrameworkV0Test, RescindOnRemovingRoles)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Expect an offer exactly once (after subscribing).
  Future<std::vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_EQ(offers->size(), 1u);

  // Set up expectations for things that should happen after role removal.

  // The offer for the removed role should be rescinded.
  Future<OfferID> rescindedOfferId;
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  // recoverResources() should be called.
  //
  // TODO(asekretenko): Add a more in-depth check that
  // the allocator does what it should.
  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Remove the framework from all roles via update.
  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  update.clear_roles();
  *update.mutable_id() = frameworkId.get();

  driver.updateFramework(update, {}, {});

  AWAIT_READY(rescindedOfferId);
  AWAIT_READY(recoverResources);

  EXPECT_EQ(offers->front().id(), rescindedOfferId.get());

  // After that, nothing of interest should happen within an allocation
  // interval: no more offers and no more rescinding.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  driver.stop();
  driver.join();
}


// Test that framework can suppress roles via updateFramework():
// - start a master, a driver and a slave
// - wait for offer
// - update suppressed roles via updateFramework
// - add a new slave and make sure we get no offers after that.
TEST_F(UpdateFrameworkV0Test, SuppressedRoles)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  v1::MockMasterAPISubscriber masterAPISubscriber;
  AWAIT_READY(masterAPISubscriber.subscribe(master.get()->pid));

  Future<Nothing> secondAgentAdded;
  EXPECT_CALL(masterAPISubscriber, agentAdded(_))
    .WillOnce(Return())
    .WillOnce(FutureSatisfy(&secondAgentAdded));

  Owned<MasterDetector> detector = master->get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Expect an offer EXACTLY once (after subscribing and before adding
  // the second slave).
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  // Suppress all roles via UPDATE_FRAMEWORK.
  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  *update.mutable_id() = frameworkId.get();

  vector<string> suppressedRoles(
    update.roles().begin(), update.roles().end());

  driver.updateFramework(update, suppressedRoles, {});

  // Ensure that the allocator processes the update, so that this test
  // does not rely on Master maintaining an ordering between scheduler API calls
  // processing and agent registration.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  Try<Owned<cluster::Slave>> newSlave = StartSlave(detector.get());
  ASSERT_SOME(newSlave);

  AWAIT_READY(secondAgentAdded);

  // After the agent has been added, no offers should be generated
  // within an allocation interval.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  driver.stop();
  driver.join();
}


// Test that when a framework removes a role from the
// suppressed roles, it clears filters (same as REVIVE):
// - start a master, a driver and an agent
// - wait for offer and decline it for a long timeout
// - add / remove suppressed roles via updateFramework
// - ensure we get an offer for the agent again
TEST_F(UpdateFrameworkV0Test, UnsuppressClearsFilters)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master->get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1->size());

  Filters filters;
  filters.set_refuse_seconds(Days(1).secs());

  driver.declineOffer(offers1->at(0).id(), filters);

  // Suppress and unsuppress the role.
  FrameworkInfo update = DEFAULT_FRAMEWORK_INFO;
  *update.mutable_id() = frameworkId.get();

  vector<string> suppressedRoles(
    update.roles().begin(), update.roles().end());

  driver.updateFramework(update, suppressedRoles, {});
  driver.updateFramework(update, {}, {});

  // Now the previously declined agent should be re-offered.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers2);

  driver.stop();
  driver.join();
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
