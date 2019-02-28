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
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;
using mesos::internal::protobuf::createLabel;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Conflict;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {


class ReservationEndpointsTest : public MesosTest
{
public:
  // Set up the master flags such that it allows registration of the framework
  // created with 'createFrameworkInfo'.
  master::Flags CreateMasterFlags() override
  {
    // Turn off periodic allocations to avoid the race between
    // `HierarchicalAllocator::updateAvailable()` and periodic allocations.
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Seconds(1000);
    flags.roles = createFrameworkInfo().roles(0);
    return flags;
  }

  // Returns a FrameworkInfo with role, "role".
  FrameworkInfo createFrameworkInfo()
  {
    FrameworkInfo info = DEFAULT_FRAMEWORK_INFO;
    info.set_roles(0, "role");
    return info;
  }

  string createRequestBody(
      const SlaveID& slaveId, const RepeatedPtrField<Resource>& resources) const
  {
    return strings::format(
        "slaveId=%s&resources=%s",
        slaveId.value(),
        JSON::protobuf(resources)).get();
  }
};


// This tests that an operator can reserve/unreserve available resources.
TEST_F(ReservationEndpointsTest, AvailableResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  // The filter to decline the offer "forever".
  Filters filtersForever;
  filtersForever.set_refuse_seconds(1000);

  // Decline the offer "forever" in order to deallocate resources.
  driver.declineOffer(offer.id(), filtersForever);

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an operator can reserve offered resources by rescinding the
// outstanding offers.
TEST_F(ReservationEndpointsTest, ReserveOfferedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an operator can unreserve offered resources by rescinding the
// outstanding offers.
TEST_F(ReservationEndpointsTest, UnreserveOfferedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an operator can reserve a mix of available and offered
// resources by rescinding the outstanding offers.
TEST_F(ReservationEndpointsTest, ReserveAvailableAndOfferedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources available = Resources::parse("cpus:1;mem:128").get();
  Resources offered = Resources::parse("mem:384").get();

  Resources total = available + offered;
  Resources dynamicallyReserved =
    total.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Expect one TASK_STARTING and one TASK_RUNNING update
  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // We want to get the cluster in a state where 'available' resources are left
  // in the allocator, and 'offered' resources are offered to the framework.
  // To achieve this state, we perform the following steps:
  //   (1) Receive an offer containing 'total' = 'available' + 'offered'.
  //   (2) Launch a "forever-running" task with 'available' resources.
  //   (3) Summon an offer containing 'offered'.
  //   (4) Kill the task, which recovers 'available' resources.

  // Expect to receive 'available + offered' resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(available + offered, frameworkInfo.roles(0))));

  // Launch a task on the 'available' resources portion of the offer, which
  // recovers 'offered' resources portion.
  TaskInfo taskInfo = createTask(offer.slave_id(), available, "sleep 1000");

  // Expect a TASK_STARTING and a TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  // Wait for update acks.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  // Summon an offer to receive the 'offered' resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(offered, frameworkInfo.roles(0))));

  // Kill the task running on 'available' resources to make it available.
  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  // Send a KillTask message to the master.
  driver.killTask(taskInfo.task_id());

  AWAIT_READY(statusUpdate);
  ASSERT_EQ(TaskState::TASK_KILLED, statusUpdate->state());

  // At this point, we have 'available' resources in the allocator, and
  // 'offered' resources offered to the framework.

  // Expect offers to be rescinded and recovered!
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillRepeatedly(Return());

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an operator can unreserve a mix of available and offered
// resources by rescinding the outstanding offers.
TEST_F(ReservationEndpointsTest, UnreserveAvailableAndOfferedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources available = Resources::parse("cpus:1;mem:128").get();
  available = available.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Resources offered = Resources::parse("mem:384").get();
  offered = offered.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Resources total = available + offered;
  Resources unreserved = total.toUnreserved();

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, total));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // We want to get the cluster in a state where 'available' resources are left
  // in the allocator, and 'offered' resources are offered to the framework.
  // To achieve this state, we perform the following steps:
  //   (1) Receive an offer containing 'total' = 'available' + 'offered'.
  //   (2) Launch a "forever-running" task with 'available' resources.
  //   (3) Summon an offer containing 'offered'.
  //   (4) Kill the task, which recovers 'available' resources.

  // Expect to receive 'available + offered' resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(available + offered, frameworkInfo.roles(0))));

  // Launch a task on the 'available' resources portion of the offer, which
  // recovers 'offered' resources portion.
  TaskInfo taskInfo = createTask(offer.slave_id(), available, "sleep 1000");

  // Expect a TASK_STARTING and a TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  // Wait for update acks from TASK_STARTING and TASK_RUNNING.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  // Summon an offer to receive the 'offered' resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(offered, frameworkInfo.roles(0))));

  // Kill the task running on 'available' resources to make it available.
  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  // Send a KillTask message to the master.
  driver.killTask(taskInfo.task_id());

  AWAIT_READY(statusUpdate);
  ASSERT_EQ(TaskState::TASK_KILLED, statusUpdate->state());

  // At this point, we have 'available' resources in the allocator, and
  // 'offered' resources offered to the framework.

  // Expect offers to be rescinded and recovered!
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillRepeatedly(Return());

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, total));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that attempts to reserve/unreserve labeled resources
// behave as expected.
TEST_F(ReservationEndpointsTest, LabeledResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";
  Resources totalSlaveResources =
    Resources::parse(slaveFlags.resources.get()).get();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Labels labels1;
  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));

  Labels labels2;
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources labeledResources1 =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal(), labels1));
  Resources labeledResources2 =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal(), labels2));

  // Make two resource reservations with different labels.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, labeledResources2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  Resources offeredResources = Resources(offer.resources());
  EXPECT_TRUE(offeredResources.contains(
      allocatedResources(labeledResources1, frameworkInfo.roles(0))));
  EXPECT_TRUE(offeredResources.contains(
      allocatedResources(labeledResources2, frameworkInfo.roles(0))));

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  // Unreserve one of the labeled reservations.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  offeredResources = Resources(offer.resources());
  EXPECT_FALSE(offeredResources.contains(
      allocatedResources(totalSlaveResources, frameworkInfo.roles(0))));
  EXPECT_TRUE(offeredResources.contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));
  EXPECT_FALSE(offeredResources.contains(
      allocatedResources(labeledResources1, frameworkInfo.roles(0))));
  EXPECT_TRUE(offeredResources.contains(
      allocatedResources(labeledResources2, frameworkInfo.roles(0))));

  // Now that the first labeled reservation has been unreserved,
  // attempting to unreserve it again should fail.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  // Unreserve the other labeled reservation.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, labeledResources2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  offeredResources = Resources(offer.resources());

  EXPECT_TRUE(offeredResources.contains(
      allocatedResources(totalSlaveResources, frameworkInfo.roles(0))));
  EXPECT_FALSE(offeredResources.contains(
      allocatedResources(labeledResources1, frameworkInfo.roles(0))));
  EXPECT_FALSE(offeredResources.contains(
      allocatedResources(labeledResources2, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an attempt to reserve or unreserve an invalid resource will
// return a Bad Request response.
TEST_F(ReservationEndpointsTest, InvalidResource)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  // This resource is marked for the default "*" role and it also has a
  // `ReservationInfo`, which is not allowed.
  Try<Resource> resource = Resources::parse("cpus", "4", "*");
  ASSERT_SOME(resource);
  resource->add_reservations()->CopyFrom(
      createDynamicReservationInfo("*", DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  // We construct the body manually here because it's difficult to construct a
  // `Resources` object that contains an invalid `Resource`, and our helper
  // function `createRequestBody` accepts `Resources`.
  string body = strings::format(
        "slaveId=%s&resources=[%s]",
        slaveId.value(),
        JSON::protobuf(resource.get())).get();

  {
    Future<Response> response =
      process::http::post(master.get()->pid, "reserve", headers, body);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    ASSERT_TRUE(strings::contains(
        response->body,
        "Invalid reservation: role \"*\" cannot be reserved"));
  }

  {
    Future<Response> response =
      process::http::post(master.get()->pid, "unreserve", headers, body);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    ASSERT_TRUE(strings::contains(
        response->body,
        "Invalid reservation: role \"*\" cannot be reserved"));
  }
}


// This tests that an attempt to reserve/unreserve more resources than available
// results in a 'Conflict' HTTP error.
TEST_F(ReservationEndpointsTest, InsufficientResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:4;mem:4096").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = createRequestBody(slaveId, dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get()->pid, "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  response = process::http::post(master.get()->pid, "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);
}


// This tests that an attempt to reserve with no authorization header results in
// an 'Unauthorized' HTTP error.
TEST_F(ReservationEndpointsTest, NoHeader)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      None(),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      None(),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// This tests that an attempt to reserve with bad credentials results in an
// 'Unauthorized' HTTP error.
TEST_F(ReservationEndpointsTest, BadCredentials)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Credential credential;
  credential.set_principal("bad-principal");
  credential.set_secret("bad-secret");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo("role", DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(credential);
  string body = createRequestBody(slaveId, dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get()->pid, "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  response = process::http::post(master.get()->pid, "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// This tests that correct setup of Reserve/Unreserve ACLs allows
// the operator to perform reserve/unreserve operations successfully.
TEST_F(ReservationEndpointsTest, GoodReserveAndUnreserveACL)
{
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can reserve resources for any role.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can unreserve its own resources.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  unreserve->mutable_reserver_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        createFrameworkInfo().roles(0), DEFAULT_CREDENTIAL.principal()));

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      headers,
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Unreserve the resources.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      headers,
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
}


// This tests that correct setup of `ReserveResources` ACLs allows the operator
// to perform reserve operations for multiple roles successfully.
TEST_F(ReservationEndpointsTest, GoodReserveACLMultipleRoles)
{
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can reserve resources for any role.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 =
    unreserved.pushReservation(createDynamicReservationInfo(
        "jedi_master", DEFAULT_CREDENTIAL.principal()));
  Resources dynamicallyReserved2 =
    unreserved.pushReservation(createDynamicReservationInfo(
        "sith_lord", DEFAULT_CREDENTIAL.principal()));
  Resources dynamicallyReservedMultipleRoles =
    dynamicallyReserved1 + dynamicallyReserved2;

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReservedMultipleRoles));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
}


// This tests that an incorrect set-up of Reserve ACL disallows the
// operator from performing reserve operations.
TEST_F(ReservationEndpointsTest, BadReserveACL)
{
  ACLs acls;

  // This ACL asserts that ANY principal can reserve NONE,
  // i.e. no principal can reserve resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        createFrameworkInfo().roles(0), DEFAULT_CREDENTIAL.principal()));

  // Attempt to reserve the resources.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      headers,
      createRequestBody(slaveId, dynamicallyReserved));

  // Expect a failed authorization.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
}


// This tests that correct set-up of Unreserve ACLs disallows the
// operator from performing unreserve operations.
TEST_F(ReservationEndpointsTest, BadUnreserveACL)
{
  ACLs acls;

  // This ACL asserts that ANY principal can unreserve NONE,
  // i.e. no principals can unreserve anything.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  unreserve->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        createFrameworkInfo().roles(0), DEFAULT_CREDENTIAL.principal()));

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      headers,
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Attempt to unreserve the resources.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      headers,
      createRequestBody(slaveId, dynamicallyReserved));

  // Expect a failed authorization.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
}


// Tests that reserve operations will fail if multiple roles are included in a
// request, while the principal attempting the reservation is not authorized to
// reserve for one of them.
TEST_F(ReservationEndpointsTest, BadReserveACLMultipleRoles)
{
  const string AUTHORIZED_ROLE = "panda";
  const string UNAUTHORIZED_ROLE = "giraffe";

  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can reserve resources for `AUTHORIZED_ROLE`.
  mesos::ACL::ReserveResources* reserve1 = acls.add_reserve_resources();
  reserve1->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve1->mutable_roles()->add_values(AUTHORIZED_ROLE);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // cannot reserve resources for any other role.
  mesos::ACL::ReserveResources* reserve2 = acls.add_reserve_resources();
  reserve2->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve2->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 =
    unreserved.pushReservation(createDynamicReservationInfo(
        AUTHORIZED_ROLE, DEFAULT_CREDENTIAL.principal()));
  Resources dynamicallyReserved2 =
    unreserved.pushReservation(createDynamicReservationInfo(
        UNAUTHORIZED_ROLE, DEFAULT_CREDENTIAL.principal()));
  Resources dynamicallyReservedMultipleRoles =
    dynamicallyReserved1 + dynamicallyReserved2;

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReservedMultipleRoles));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
}


// This tests that an attempt to reserve with no 'slaveId' results in a
// 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NoSlaveId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo("role", DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body =
    "resources=" +
    stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(dynamicallyReserved)));

  Future<Response> response =
    process::http::post(master.get()->pid, "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  response = process::http::post(master.get()->pid, "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This tests that an attempt to reserve with no 'resources' results in a
// 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NoResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = "slaveId=" + slaveId.value();

  Future<Response> response =
    process::http::post(master.get()->pid, "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  response = process::http::post(master.get()->pid, "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This tests that an attempt to reserve with a non-matching principal results
// in a 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NonMatchingPrincipal)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo("role", "badPrincipal"));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// Tests the situation where framework and HTTP authentication are disabled
// and no ACLs are set in the master.
TEST_F(ReservationEndpointsTest, ReserveAndUnreserveNoAuthentication)
{
  FrameworkInfo frameworkInfo = createFrameworkInfo();

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;
  masterFlags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create an agent.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  Resources dynamicallyReservedWithNoPrincipal = unreserved.pushReservation(
      createDynamicReservationInfo(frameworkInfo.roles(0)));

  // Try a reservation with no principal in `ReservationInfo` and no
  // authentication headers.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      None(),
      createRequestBody(slaveId, dynamicallyReservedWithNoPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Try to unreserve with no principal in `ReservationInfo` and no
  // authentication headers.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      None(),
      createRequestBody(slaveId, dynamicallyReservedWithNoPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  Resources dynamicallyReservedWithPrincipal =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  // Try a reservation with a principal in `ReservationInfo` and no
  // authentication headers.
  response = process::http::post(
      master.get()->pid,
      "reserve",
      None(),
      createRequestBody(slaveId, dynamicallyReservedWithPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Try to unreserve with a principal in `ReservationInfo` and no
  // authentication headers.
  response = process::http::post(
      master.get()->pid,
      "unreserve",
      None(),
      createRequestBody(slaveId, dynamicallyReservedWithPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
}


// This test checks that two resource reservations for the same role
// at the same agent that use different principals are not "merged"
// into a single reserved resource.
TEST_F(ReservationEndpointsTest, DifferentPrincipalsSameRole)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Resources dynamicallyReserved2 =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL_2.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, dynamicallyReserved1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
      createRequestBody(slaveId, dynamicallyReserved2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];
  Resources resources = Resources(offer.resources());

  EXPECT_TRUE(resources.contains(
      allocatedResources(dynamicallyReserved1, frameworkInfo.roles(0))));
  EXPECT_TRUE(resources.contains(
      allocatedResources(dynamicallyReserved2, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test verifies that unreserved resources, dynamic reservations, allocated
// resources per each role are reflected in the agent's "/state" endpoint.
TEST_F(ReservationEndpointsTest, AgentStateEndpointResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:4;mem:2048;disk:4096;cpus(role):2;mem(role):512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(updateSlaveMessage);
  const SlaveID& slaveId = updateSlaveMessage->slave_id();

  Resources unreserved = Resources::parse("cpus:1;mem:512;disk:1024").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo("role1", DEFAULT_CREDENTIAL.principal()));

  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "reserve",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        createRequestBody(slaveId, dynamicallyReserved));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  }

  // Now verify the reservations from the agent's /state endpoint. We wait for
  // the agent to receive and process the `ApplyOperationMessage` and respond
  // with an initial operation status update.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // Make sure CheckpointResourcesMessage handling is completed
  // before proceeding.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  Resources taskResources = Resources::parse(
      "cpus(role):2;mem(role):512;cpus:2;mem:1024").get();

  TaskInfo task = createTask(offer.slave_id(), taskResources, "sleep 1000");

  driver.acceptOffers({offer.id()}, {LAUNCH({task})});

  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  Future<Response> response = process::http::get(
      agent.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  {
    JSON::Value expected = JSON::parse(
        R"~(
        {
          "role": {
            "cpus": 2.0,
            "disk": 0.0,
            "gpus": 0.0,
            "mem": 512.0
          },
          "role1": {
            "cpus": 1.0,
            "disk": 1024.0,
            "gpus": 0.0,
            "mem": 512.0
          }
        })~").get();

    EXPECT_EQ(expected, state.values["reserved_resources"]);
  }

  {
    JSON::Value expected = JSON::parse(
        R"~(
        {
          "role": {
            "cpus": 2.0,
            "disk": 0.0,
            "gpus": 0.0,
            "mem": 512.0
          }
        })~").get();

    EXPECT_EQ(expected, state.values["reserved_resources_allocated"]);
  }

  {
    JSON::Value expected = JSON::parse(
        R"~(
        {
          "cpus": 3.0,
          "disk": 3072.0,
          "gpus": 0.0,
          "mem": 1536.0,
          "ports": "[31000-32000]"
        })~").get();

    EXPECT_EQ(expected, state.values["unreserved_resources"]);
  }

  {
    // NOTE: executor consumes extra 0.1 cpus and 32.0 mem
    JSON::Value expected = JSON::parse(
        R"~(
        {
          "cpus": 2.1,
          "disk": 0.0,
          "gpus": 0.0,
          "mem": 1056.0
        })~").get();

    EXPECT_EQ(expected, state.values["unreserved_resources_allocated"]);
  }

  {
    JSON::Value expected = JSON::parse(strings::format(
        R"~(
        {
          "role": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": {
                "value": 2.0
              },
              "role": "role",
              "reservations": [
                {
                  "role": "role",
                  "type": "STATIC"
                }
              ]
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": {
                "value": 512.0
              },
              "role": "role",
              "reservations": [
                {
                  "role": "role",
                  "type": "STATIC"
                }
              ]
            }
          ],
          "role1": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": {
                "value": 1.0
              },
              "role": "role1",
              "reservation": {
                "principal": "%s"
              },
              "reservations": [
                {
                  "principal": "%s",
                  "role": "role1",
                  "type": "DYNAMIC"
                }
              ]
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": {
                "value": 512.0
              },
              "role": "role1",
              "reservation": {
                "principal": "%s"
              },
              "reservations": [
                {
                  "principal": "%s",
                  "role": "role1",
                  "type": "DYNAMIC"
                }
              ]
            },
            {
              "name": "disk",
              "type": "SCALAR",
              "scalar": {
                "value": 1024.0
              },
              "role": "role1",
              "reservation": {
                "principal": "%s"
              },
              "reservations": [
                {
                  "principal": "%s",
                  "role": "role1",
                  "type": "DYNAMIC"
                }
              ]
            }
          ]
        })~",
        DEFAULT_CREDENTIAL.principal(), // Three occurrences of '%s' above.
        DEFAULT_CREDENTIAL.principal(),
        DEFAULT_CREDENTIAL.principal(),
        DEFAULT_CREDENTIAL.principal(),
        DEFAULT_CREDENTIAL.principal(),
        DEFAULT_CREDENTIAL.principal()).get()).get();

    EXPECT_EQ(expected, state.values["reserved_resources_full"]);
  }

  {
    JSON::Value expected = JSON::parse(
        R"~(
        [
          {
            "name": "cpus",
            "role": "*",
            "scalar": {
              "value": 3.0
            },
            "type": "SCALAR"
          },
          {
            "name": "mem",
            "role": "*",
            "scalar": {
              "value": 1536.0
            },
            "type": "SCALAR"
          },
          {
            "name": "disk",
            "role": "*",
            "scalar": {
              "value": 3072.0
            },
            "type": "SCALAR"
          },
          {
            "name": "ports",
            "role": "*",
            "ranges": {
              "range": [
                {
                  "begin": 31000,
                  "end": 32000
                }
              ]
            },
            "type": "RANGES"
          }
        ])~").get();

    EXPECT_EQ(expected, state.values["unreserved_resources_full"]);
  }

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
