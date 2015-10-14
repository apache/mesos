/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/base64.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using std::string;
using std::vector;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::Conflict;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::SaveArg;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


// Converts a 'RepeatedPtrField<Resource>' to a 'JSON::Array'.
// TODO(mpark): Generalize this to 'JSON::protobuf(RepeatedPtrField<T>)'.
JSON::Array toJSONArray(
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  JSON::Array array;

  array.values.reserve(resources.size());

  foreach (const Resource& resource, resources) {
    array.values.push_back(JSON::Protobuf(resource));
  }

  return array;
}


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

  hashmap<string, string> createBasicAuthHeaders(
      const Credential& credential) const
  {
    return hashmap<string, string>{{
      "Authorization",
      "Basic " +
        base64::encode(credential.principal() + ":" + credential.secret())
    }};
  }

  string createRequestBody(
      const SlaveID& slaveId, const Resources& resources) const
  {
    return strings::format(
        "slaveId=%s&resources=%s",
        slaveId.value(),
        toJSONArray(resources)).get();
  }
};


// TODO(mpark): Add tests for ACLs once they are introduced.


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

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

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
  // resources that this framework currently holding onto.
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

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

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

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded!
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

  driver.start();

  // We want to get the cluster in a state where 'available' resources are left
  // in the allocator, and 'offered' resources are offered to the framework.
  // To achieve this state, we perform the following steps:
  //   (1) Summon an offer containing 'total' = 'available' + 'offered'.
  //   (2) Launch a "forever-running" task with 'available' resources.
  //   (3) Summon an offer containing 'offered'.
  //   (4) Kill the task, which recovers 'available' resources.

  // Summon an offer and expect to receive 'available + offered' resources.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

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
  // resources that this framework currently holding onto.
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

  driver.start();

  // We want to get the cluster in a state where 'available' resources are left
  // in the allocator, and 'offered' resources are offered to the framework.
  // To achieve this state, we perform the following steps:
  //   (1) Summon an offer containing 'total' = 'available' + 'offered'.
  //   (2) Launch a "forever-running" task with 'available' resources.
  //   (3) Summon an offer containing 'offered'.
  //   (4) Kill the task, which recovers 'available' resources.

  // Summon an offer and expect to receive 'available + offered' resources.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

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
  // resources that this framework currently holding onto.
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

  hashmap<string, string> headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = createRequestBody(slaveId.get(), dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  Shutdown();
}


// This tests that an attempt to reserve with no authorization header results in
// a 'Unauthorized' HTTP error.
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
      Unauthorized("Mesos master").status,
      response);

  response = process::http::post(
      master.get(),
      "unreserve",
      None(),
      createRequestBody(slaveId.get(), dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized("Mesos master").status,
      response);

  Shutdown();
}


// This tests that an attempt to reserve with bad credentials results in a
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

  hashmap<string, string> headers = createBasicAuthHeaders(credential);
  string body = createRequestBody(slaveId.get(), dynamicallyReserved);

  Future<Response> response =
    process::http::post(master.get(), "reserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized("Mesos master").status,
      response);

  response = process::http::post(master.get(), "unreserve", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized("Mesos master").status,
      response);

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

  hashmap<string, string> headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = "resources=" + stringify(toJSONArray(dynamicallyReserved));

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

  hashmap<string, string> headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
