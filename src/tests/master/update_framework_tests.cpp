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

#include <google/protobuf/util/message_differencer.h>

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

#include "internal/evolve.hpp"

#include "master/allocator/mesos/allocator.hpp"
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

using std::string;

using testing::_;
using testing::AtMost;
using testing::Return;


namespace mesos {
namespace internal {
namespace tests {
namespace v1 {
namespace scheduler {


class UpdateFrameworkTest : public MesosTest {};


static Future<APIResult> callUpdateFramework(
    Mesos* mesos,
    const FrameworkInfo& info)
{
  CHECK(info.has_id());

  Call call;
  call.set_type(Call::UPDATE_FRAMEWORK);
  *call.mutable_framework_id() = info.id();
  *call.mutable_update_framework()->mutable_framework_info() = info;
  return mesos->call(call);
}


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


static FrameworkInfo changeAllMutableFields(const FrameworkInfo& oldInfo)
{
  CHECK_EQ(FrameworkInfo::descriptor()->field_count(), 13)
    << "After adding a new mutable field to FrameworkInfo, please make sure "
    << "that this function modifies this field";

  FrameworkInfo newInfo = oldInfo;

  *newInfo.mutable_name() += "_foo";
  newInfo.set_failover_timeout(newInfo.failover_timeout() + 1000.0);
  *newInfo.mutable_hostname() += ".foo";
  *newInfo.mutable_webui_url() += "/foo";

  newInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::REGION_AWARE);

  mesos::v1::Label* newLabel = newInfo.mutable_labels()->add_labels();
  *newLabel->mutable_key() = "UPDATE_FRAMEWORK_KEY";
  *newLabel->mutable_value() = "UPDATE_FRAMEWORK_VALUE";

  // TODO(asekretenko): Test update of `role` with a non-MULTI_ROLE framework.
  newInfo.add_roles("new_role");

  CHECK(newInfo.offer_filters().count("new_role") == 0);
  (*newInfo.mutable_offer_filters())["new_role_without_resources"] =
    mesos::v1::OfferFilters();

  return newInfo;
}


static Option<string> diff(
    const FrameworkInfo& lhs, const FrameworkInfo& rhs)
{
  const google::protobuf::Descriptor* descriptor = FrameworkInfo::descriptor();
  google::protobuf::util::MessageDifferencer differencer;

  differencer.TreatAsSet(descriptor->FindFieldByName("capabilities"));
  differencer.TreatAsSet(descriptor->FindFieldByName("roles"));

  string result;
  differencer.ReportDifferencesToString(&result);

  if (differencer.Compare(lhs, rhs)) {
    return None();
  }

  return result;
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
  EXPECT_NONE(diff(frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(diff(update, expected));
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
  EXPECT_NONE(diff(frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(diff(update, expected));
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
  EXPECT_NONE(diff(frameworks->frameworks(0).framework_info(), expected));

  // Sanity check for diff()
  EXPECT_SOME(diff(update, expected));
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

  EXPECT_NONE(diff(frameworkInfo, update));

  AWAIT_READY(updateFrameworkMessage);
  EXPECT_NONE(diff(evolve(updateFrameworkMessage->framework_info()), update));

  AWAIT_READY(frameworkUpdated);
  EXPECT_NONE(diff(frameworkUpdated->framework().framework_info(), update));
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


} // namespace scheduler {
} // namespace v1 {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
