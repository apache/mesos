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
#include <process/pid.hpp>

#include <stout/base64.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;
using mesos::internal::protobuf::createLabel;
using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::Conflict;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using testing::_;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {


class ReservationEndpointsTest : public MesosTest
{
public:
  // Set up the master flags such that it allows registration of the framework
  // created with 'createFrameworkInfo'.
  virtual master::Flags CreateMasterFlags()
  {
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Milliseconds(50);
    flags.roles = createFrameworkInfo().role();
    return flags;
  }

  // Returns a FrameworkInfo with role, "role".
  FrameworkInfo createFrameworkInfo()
  {
    FrameworkInfo info = DEFAULT_FRAMEWORK_INFO;
    info.set_role("role");
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
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  Future<Nothing> recoverResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverResources)));

  // The filter to decline the offer "forever".
  Filters filtersForever;
  filtersForever.set_refuse_seconds(1000);

  // Decline the offer "forever" in order to deallocate resources.
  driver.declineOffer(offer.id(), filtersForever);

  AWAIT_READY(recoverResources);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // Ignore subsequent `recoverResources` calls triggered from recovering the
  // resources that this framework is currently holding onto.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an operator can reserve offered resources by rescinding the
// outstanding offers.
TEST_F(ReservationEndpointsTest, ReserveOfferedResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an operator can unreserve offered resources by rescinding the
// outstanding offers.
TEST_F(ReservationEndpointsTest, UnreserveOfferedResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an operator can reserve a mix of available and offered
// resources by rescinding the outstanding offers.
TEST_F(ReservationEndpointsTest, ReserveAvailableAndOfferedResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = CreateMasterFlags();
  // Turn off allocation. We're doing it manually.
  masterFlags.allocation_interval = Seconds(1000);

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources available = Resources::parse("cpus:1;mem:128").get();
  Resources offered = Resources::parse("mem:384").get();

  Resources total = available + offered;
  Resources dynamicallyReserved = total.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(available + offered));

  // Launch a task on the 'available' resources portion of the offer, which
  // recovers 'offered' resources portion.
  TaskInfo taskInfo = createTask(offer.slave_id(), available, "sleep 1000");

  // Expect a TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> recoverUnusedResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverUnusedResources)));

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  // Wait for TASK_RUNNING update ack and for the resources to be recovered.
  AWAIT_READY(_statusUpdateAcknowledgement);
  AWAIT_READY(recoverUnusedResources);

  // Summon an offer to receive the 'offered' resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(offered));

  // Kill the task running on 'available' resources to make it available.
  EXPECT_CALL(sched, statusUpdate(_, _));

  // Wait for the used resources to be recovered.
  Future<Resources> availableResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureArg<2>(&availableResources)));

  // Send a KillTask message to the master.
  driver.killTask(taskInfo.task_id());

  EXPECT_TRUE(availableResources.get().contains(available));

  // At this point, we have 'available' resources in the allocator, and
  // 'offered' resources offered to the framework.

  // Expect an offer to be rescinded and recovered!
  EXPECT_CALL(sched, offerRescinded(_, _));
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoDefault());

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // Ignore subsequent `recoverResources` calls triggered from recovering the
  // resources that this framework is currently holding onto.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an operator can unreserve a mix of available and offered
// resources by rescinding the outstanding offers.
TEST_F(ReservationEndpointsTest, UnreserveAvailableAndOfferedResources)
{
  TestAllocator<> allocator;

  master::Flags masterFlags = CreateMasterFlags();
  // Turn off allocation. We're doing it manually.
  masterFlags.allocation_interval = Seconds(1000);

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources available = Resources::parse("cpus:1;mem:128").get();
  available = available.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Resources offered = Resources::parse("mem:384").get();
  offered = offered.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Resources total = available + offered;
  Resources unreserved = total.flatten();

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), total));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(available + offered));

  // Launch a task on the 'available' resources portion of the offer, which
  // recovers 'offered' resources portion.
  TaskInfo taskInfo = createTask(offer.slave_id(), available, "sleep 1000");

  // Expect a TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> recoverUnusedResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverUnusedResources)));

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  // Wait for TASK_RUNNING update ack and for the resources to be recovered.
  AWAIT_READY(_statusUpdateAcknowledgement);
  AWAIT_READY(recoverUnusedResources);

  // Summon an offer to receive the 'offered' resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(offered));

  // Kill the task running on 'available' resources to make it available.
  EXPECT_CALL(sched, statusUpdate(_, _));

  // Wait for the used resources to be recovered.
  Future<Resources> availableResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureArg<2>(&availableResources)));

  // Send a KillTask message to the master.
  driver.killTask(taskInfo.task_id());

  EXPECT_TRUE(availableResources.get().contains(available));

  // At this point, we have 'available' resources in the allocator, and
  // 'offered' resources offered to the framework.

  // Expect an offer to be rescinded and recovered!
  EXPECT_CALL(sched, offerRescinded(_, _));
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoDefault());

  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), total));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // Ignore subsequent `recoverResources` calls triggered from recovering the
  // resources that this framework is currently holding onto.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that attempts to reserve/unreserve labeled resources
// behave as expected.
TEST_F(ReservationEndpointsTest, LabeledResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";
  Resources totalSlaveResources =
    Resources::parse(slaveFlags.resources.get()).get();

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Labels labels1;
  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));

  Labels labels2;
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources labeledResources1 = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal(), labels1));
  Resources labeledResources2 = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal(), labels2));

  // Make two resource reservations with different labels.
  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), labeledResources2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  Resources offeredResources = Resources(offer.resources());
  EXPECT_TRUE(offeredResources.contains(labeledResources1));
  EXPECT_TRUE(offeredResources.contains(labeledResources2));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  // Unreserve one of the labeled reservations.
  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  offeredResources = Resources(offer.resources());
  EXPECT_FALSE(offeredResources.contains(totalSlaveResources));
  EXPECT_TRUE(offeredResources.contains(unreserved));
  EXPECT_FALSE(offeredResources.contains(labeledResources1));
  EXPECT_TRUE(offeredResources.contains(labeledResources2));

  // Now that the first labeled reservation has been unreserved,
  // attempting to unreserve it again should fail.
  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), labeledResources1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(_, _));

  // Unreserve the other labeled reservation.
  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), labeledResources2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  offeredResources = Resources(offer.resources());

  EXPECT_TRUE(offeredResources.contains(totalSlaveResources));
  EXPECT_FALSE(offeredResources.contains(labeledResources1));
  EXPECT_FALSE(offeredResources.contains(labeledResources2));

  // Ignore subsequent `recoverResources` calls triggered from recovering the
  // resources that this framework is currently holding onto.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an attempt to reserve/unreserve more resources than available
// results in a 'Conflict' HTTP error.
TEST_F(ReservationEndpointsTest, InsufficientResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:4;mem:4096").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = createRequestBody(slaveId.get(), dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  Shutdown();
}


// This tests that an attempt to reserve with no authorization header results in
// an 'Unauthorized' HTTP error.
TEST_F(ReservationEndpointsTest, NoHeader)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      None(),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  response = process::http::post(
      master.get(),
      "unreserve",
      None(),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  Shutdown();
}


// This tests that an attempt to reserve with bad credentials results in an
// 'Unauthorized' HTTP error.
TEST_F(ReservationEndpointsTest, BadCredentials)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  Credential credential;
  credential.set_principal("bad-principal");
  credential.set_secret("bad-secret");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      "role", createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(credential);
  string body = createRequestBody(slaveId.get(), dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  Shutdown();
}


// This tests that correct setup of Reserve/Unreserve ACLs allows
// the operator to perform reserve/unreserve operations successfully.
TEST_F(ReservationEndpointsTest, GoodReserveAndUnreserveACL)
{
  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that the DEFAULT_CREDENTIAL's
  // principal can reserve ANY resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the DEFAULT_CREDENTIAL's
  // principal can unreserve its own resources.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  unreserve->mutable_reserver_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      headers,
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Unreserve the resources.
  response = process::http::post(
      master.get(),
      "unreserve",
      headers,
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Shutdown();
}


// This tests that an incorrect set-up of Reserve ACL disallows the
// operator from performing reserve operations.
TEST_F(ReservationEndpointsTest, BadReserveACL)
{
  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that ANY principal can reserve NONE,
  // i.e. no principals can reserve anything.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::NONE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  // Attempt to reserve the resources.
  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      headers,
      createRequestBody(slaveId.get(), dynamicallyReserved));

  // Expect a failed authorization.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  Shutdown();
}


// This tests that correct set-up of Unreserve ACLs disallows the
// operator from performing unreserve operations.
TEST_F(ReservationEndpointsTest, BadUnreserveACL)
{
  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that ANY principal can unreserve NONE,
  // i.e. no principals can unreserve anything.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  unreserve->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  // Reserve the resources.
  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      headers,
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Attempt to unreserve the resources.
  response = process::http::post(
      master.get(),
      "unreserve",
      headers,
      createRequestBody(slaveId.get(), dynamicallyReserved));

  // Expect a failed authorization.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  Shutdown();
}


// This tests that an attempt to reserve with no 'slaveId' results in a
// 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NoSlaveId)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      "role", createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body =
    "resources=" +
    stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(dynamicallyReserved)));

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This tests that an attempt to reserve with no 'resources' results in a
// 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NoResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = "slaveId=" + slaveId.get().value();

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This tests that an attempt to reserve with a non-matching principal results
// in a 'BadRequest' HTTP error.
TEST_F(ReservationEndpointsTest, NonMatchingPrincipal)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.flatten("role", createReservationInfo("badPrincipal"));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This tests the situation where framework and HTTP authentication are disabled
// and no ACLs are set in the master. Currently, the reservation endpoints do
// not work in this case because the master invalidates reserve and unreserve
// operations with no authenticated principal or no principal set in
// `ReservationInfo`. In 0.28.0, these endpoints will work in this case.
//
// TODO(greggomann): Change this test for 0.28.0; see comments below.
TEST_F(ReservationEndpointsTest, ReserveAndUnreserveNoAuthentication)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  TestAllocator<> allocator;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;
  masterFlags.authenticate_http = false;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create an agent.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  Resources dynamicallyReservedWithNoPrincipal = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo());

  // Try a reservation with no principal in `ReservationInfo` and no
  // authentication headers. This will fail because currently, dynamic
  // reservations without either an authenticated principal or a principal in
  // `ReservationInfo` are invalidated.
  //
  // TODO(greggomann): Update this request for 0.28.0. This request should
  // succeed, but since `ReservationInfo.principal` is being migrated to
  // `optional`, the request is currently invalidated.
  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      None(),
      createRequestBody(slaveId.get(), dynamicallyReservedWithNoPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Create a framework that we can use to dynamically reserve some resources
  // that we will then attempt to unreserve.
  //
  // TODO(greggomann): Remove this reserving framework for 0.28.0. It will no
  // longer be needed once the HTTP reserve request succeeds.
  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Expect an offer from the agent.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  // We use this filter so that resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources dynamicallyReservedWithDefaultPrincipal =
    unreserved.flatten(
        "role", createReservationInfo(frameworkInfo.principal()));

  // Dynamically reserve resources using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReservedWithDefaultPrincipal)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the reserved resources are contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      dynamicallyReservedWithDefaultPrincipal));

  // Try to unreserve with no principal in `ReservationInfo` and no
  // authentication headers. This will fail because currently, unreserve
  // operations without either an authenticated principal or a principal in
  // `ReservationInfo` are invalidated.
  //
  // TODO(greggomann): Update this request for 0.28.0. This request should
  // succeed, but since `ReservationInfo.principal` is being migrated to
  // `optional`, the request is currently invalidated.
  response = process::http::post(
      master.get(),
      "unreserve",
      None(),
      createRequestBody(slaveId.get(), dynamicallyReservedWithNoPrincipal));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This test checks that two resource reservations for the same role
// at the same agent that use different principals are not "merged"
// into a single reserved resource.
TEST_F(ReservationEndpointsTest, DifferentPrincipalsSameRole)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Resources dynamicallyReserved2 = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL_2.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), dynamicallyReserved1));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
      createRequestBody(slaveId.get(), dynamicallyReserved2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];
  Resources resources = Resources(offer.resources());

  EXPECT_TRUE(resources.contains(dynamicallyReserved1));
  EXPECT_TRUE(resources.contains(dynamicallyReserved2));

  driver.stop();
  driver.join();

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
