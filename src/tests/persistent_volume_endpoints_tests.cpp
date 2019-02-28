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

#include <stout/none.hpp>
#include <stout/option.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

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
using testing::Not;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class PersistentVolumeEndpointsTest : public MesosTest
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

  // Returns a FrameworkInfo with role, "role1".
  FrameworkInfo createFrameworkInfo()
  {
    FrameworkInfo info = DEFAULT_FRAMEWORK_INFO;
    info.set_roles(0, "role1");
    return info;
  }

  string createRequestBody(
      const SlaveID& slaveId,
      const string& resourceKey,
      const RepeatedPtrField<Resource>& resources) const
  {
    return strings::format(
        "slaveId=%s&%s=%s",
        slaveId.value(),
        resourceKey,
        JSON::protobuf(resources)).get();
  }
};


// This tests that an operator can create a persistent volume from
// statically reserved resources, and can then destroy that volume.
TEST_F(PersistentVolumeEndpointsTest, StaticReservation)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

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
      allocatedResources(volume, frameworkInfo.roles(0))));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, destroyResponse);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that an operator can create a persistent volume from
// dynamically reserved resources, and can then destroy that volume.
TEST_F(PersistentVolumeEndpointsTest, DynamicReservation)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(*):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Offer the dynamically reserved resources to a framework. The
  // offer should be rescinded when a persistent volume is created
  // using the same resources (below).
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

  EXPECT_TRUE(Resources(offer.resources())
                .contains(allocatedResources(
                    dynamicallyReserved, frameworkInfo.roles(0))));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.roles(0),
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  // After destroying the volume, we should rescind the previous offer
  // containing the volume.
  response = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  driver.stop();
  driver.join();
}


// This tests that an attempt to create a persistent volume fails with
// a 'Conflict' HTTP error if the only available reserved resources on
// the slave have been reserved by a different role.
TEST_F(PersistentVolumeEndpointsTest, DynamicReservationRoleMismatch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(*):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

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

  ASSERT_NE(frameworkInfo.roles(0), "role2");
  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role2",
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  driver.stop();
  driver.join();
}


// This tests that an attempt to unreserve the resources used by a
// persistent volume results in a 'Conflict' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, UnreserveVolumeResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(*):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.roles(0),
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);
}


// This tests that an attempt to create or destroy a volume containing an
// invalid resource will receive a Bad Request response.
TEST_F(PersistentVolumeEndpointsTest, InvalidVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  // This volume is reserved for role "*", which is not allowed.
  Resource disk = Resources::parse("disk", "64", "*").get();
  disk.add_reservations()->CopyFrom(createDynamicReservationInfo("*"));

  Resource volume = createPersistentVolume(
      disk,
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal(),
      DEFAULT_CREDENTIAL.principal());

  // We construct the body manually here because it's difficult to construct a
  // `Resources` object that contains an invalid `Resource`, and our helper
  // function `createRequestBody` accepts `Resources`.
  string body = strings::format(
        "slaveId=%s&volumes=[%s]",
        slaveId.value(),
        JSON::protobuf(volume)).get();

  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "create-volumes",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        body);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    ASSERT_TRUE(strings::contains(
        response->body,
        "Invalid reservation: role \"*\" cannot be reserved"));
  }

  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "destroy-volumes",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        body);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    ASSERT_TRUE(strings::contains(
        response->body,
        "Invalid reservation: role \"*\" cannot be reserved"));
  }
}


// This tests that an attempt to create a volume that is larger than the
// reserved resources at the slave results in a 'Conflict' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, VolumeExceedsReservedSize)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(1025),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, createResponse);
}


// This tests that an attempt to delete a non-existent persistent
// volume results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, DeleteNonExistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, createResponse);

  // Non-existent volume ID.
  Resources badVolumeId = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id2",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", badVolumeId));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // Non-existent role.
  Resources badRole = createPersistentVolume(
      Megabytes(64),
      "role2",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", badRole));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // Size mismatch.
  Resources badSize = createPersistentVolume(
      Megabytes(128),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", badSize));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // NOTE: Two persistent volumes with different paths are considered
  // equivalent, so the destroy operation will succeed. It is unclear
  // whether this behavior is desirable (MESOS-3961).
  Resources differentPath = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path2",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", differentPath));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, destroyResponse);
}


// This tests that an attempt to create or destroy a volume with no
// authorization header results in an 'Unauthorized' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoHeader)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.roles(0),
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> response = process::http::post(
      master.get()->pid,
      "create-volumes",
      None(),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  response = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      None(),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// This tests that an attempt to create or destroy a volume with bad
// credentials results in an 'Unauthorized' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, BadCredentials)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Credential credential;
  credential.set_principal("bad-principal");
  credential.set_secret("bad-secret");

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  process::http::Headers headers = createBasicAuthHeaders(credential);
  string body = createRequestBody(slaveId, "volumes", volume);

  Future<Response> response =
    process::http::post(master.get()->pid, "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  response =
    process::http::post(master.get()->pid, "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// This tests that correct setup of CreateVolume/DestroyVolume ACLs allows an
// operator to perform volume creation/destruction operations successfully.
TEST_F(PersistentVolumeEndpointsTest, GoodCreateAndDestroyACL)
{
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create volumes for any role.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can destroy volumes that it created.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  destroy->mutable_creator_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration before we HTTP POST.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Future<Response> createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

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
      allocatedResources(volume, frameworkInfo.roles(0))));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, destroyResponse);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// Tests that correct setup of `CreateVolume` ACLs allows an operator to perform
// volume creation operations successfully when volumes for multiple roles are
// included in the request.
TEST_F(PersistentVolumeEndpointsTest, GoodCreateACLMultipleRoles)
{
  const string AUTHORIZED_ROLE_1 = "potato_head";
  const string AUTHORIZED_ROLE_2 = "gumby";

  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create volumes for any role.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources =
    "cpus:1;mem:512;disk(" + AUTHORIZED_ROLE_1 + "):1024;disk(" +
    AUTHORIZED_ROLE_2 + "):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume1 = createPersistentVolume(
      Megabytes(64),
      AUTHORIZED_ROLE_1,
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Resources volume2 = createPersistentVolume(
      Megabytes(64),
      AUTHORIZED_ROLE_2,
      "id2",
      "path2",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Resources volumesMultipleRoles = volume1 + volume2;

  Future<Response> response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volumesMultipleRoles));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
}


// This tests that an ACL prohibiting the creation of a persistent volume by a
// principal will lead to a properly failed request.
TEST_F(PersistentVolumeEndpointsTest, BadCreateAndDestroyACL)
{
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL_2`
  // cannot create persistent volumes.
  mesos::ACL::CreateVolume* cannotCreate = acls.add_create_volumes();
  cannotCreate->mutable_principals()->add_values(
      DEFAULT_CREDENTIAL_2.principal());
  cannotCreate->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* canCreate = acls.add_create_volumes();
  canCreate->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  canCreate->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL_2`
  // cannot destroy persistent volumes.
  mesos::ACL::DestroyVolume* cannotDestroy = acls.add_destroy_volumes();
  cannotDestroy->mutable_principals()->add_values(
      DEFAULT_CREDENTIAL_2.principal());
  cannotDestroy->mutable_creator_principals()->set_type(
      mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance clock to trigger agent registration before we HTTP POST.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  // The failed creation attempt.
  {
    Resources volume = createPersistentVolume(
        Megabytes(64),
        "role1",
        "id1",
        "path1",
        None(),
        None(),
        DEFAULT_CREDENTIAL_2.principal());

    Future<Response> response = process::http::post(
        master.get()->pid,
        "create-volumes",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
        createRequestBody(slaveId, "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
  }

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  // The successful creation attempt.
  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "create-volumes",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        createRequestBody(slaveId, "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  }

  FrameworkInfo frameworkInfo = createFrameworkInfo();

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
      allocatedResources(volume, frameworkInfo.roles(0))));

  // The failed destruction attempt.
  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, destroyResponse);

  driver.stop();
  driver.join();
}


// Tests that a request to create volumes will fail if volumes for multiple
// roles are included in the request and the operator is not authorized to
// create volumes for one of them.
TEST_F(PersistentVolumeEndpointsTest, BadCreateACLMultipleRoles)
{
  const string AUTHORIZED_ROLE = "potato_head";
  const string UNAUTHORIZED_ROLE = "gumby";

  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create volumes for `AUTHORIZED_ROLE`.
  mesos::ACL::CreateVolume* create1 = acls.add_create_volumes();
  create1->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create1->mutable_roles()->add_values(AUTHORIZED_ROLE);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // cannot create volumes for any other role.
  mesos::ACL::CreateVolume* create2 = acls.add_create_volumes();
  create2->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create2->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources =
    "cpus:1;mem:512;disk(" + AUTHORIZED_ROLE + "):1024;disk(" +
    UNAUTHORIZED_ROLE + "):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume1 = createPersistentVolume(
      Megabytes(64),
      AUTHORIZED_ROLE,
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Resources volume2 = createPersistentVolume(
      Megabytes(64),
      UNAUTHORIZED_ROLE,
      "id2",
      "path2",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Resources volumesMultipleRoles = volume1 + volume2;

  Future<Response> response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volumesMultipleRoles));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
}


// This tests that a request containing a credential which is not listed in the
// master for authentication will not succeed, even if a good authorization ACL
// is provided.
TEST_F(PersistentVolumeEndpointsTest, GoodCreateAndDestroyACLBadCredential)
{
  // Create a credential which will not be listed
  // for valid authentication with the master.
  Credential failedCredential;
  failedCredential.set_principal("awesome-principal");
  failedCredential.set_secret("super-secret-secret");

  ACLs acls;

  // This ACL asserts that the principal of `failedCredential`
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* failedCreate = acls.add_create_volumes();
  failedCreate->mutable_principals()->add_values(failedCredential.principal());
  failedCreate->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `failedCredential`
  // can destroy persistent volumes.
  mesos::ACL::DestroyVolume* failedDestroy = acls.add_destroy_volumes();
  failedDestroy->mutable_principals()->add_values(failedCredential.principal());
  failedDestroy->mutable_creator_principals()->set_type(
      mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* canCreate = acls.add_create_volumes();
  canCreate->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  canCreate->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance clock to trigger agent registration before we HTTP POST.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  // The failed creation attempt.
  Future<Response> createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(failedCredential),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, createResponse);

  // The successful creation attempt.
  createResponse = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

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
      allocatedResources(volume, frameworkInfo.roles(0))));

  // The failed destruction attempt.
  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(failedCredential),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, destroyResponse);

  driver.stop();
  driver.join();
}


// This test creates and destroys a volume without authentication headers and
// with authorization disabled. These requests will succeed because HTTP
// authentication is disabled, so authentication headers are not required.
// Additionally, since authorization is not enabled, any principal, including
// `None()`, can create and destroy volumes.
TEST_F(PersistentVolumeEndpointsTest, NoAuthentication)
{
  const string TEST_ROLE = "role1";

  // Create master flags that will disable authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create an agent with statically reserved disk resources to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(" + TEST_ROLE + "):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      TEST_ROLE,
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  // Make a request to create a volume with no authentication header.
  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "create-volumes",
        None(),
        createRequestBody(slaveId, "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  }

  // Make a request to destroy a volume with no authentication header.
  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "destroy-volumes",
        None(),
        createRequestBody(slaveId, "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  }
}


// This tests that an attempt to create or destroy a volume with no
// 'slaveId' results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoSlaveId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body =
    "volumes=" +
    stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(volume)));

  Future<Response> response =
    process::http::post(master.get()->pid, "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Create a volume so that a well-formed destroy attempt would succeed.
  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  response =
    process::http::post(master.get()->pid, "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This tests that an attempt to create or destroy a volume without
// the 'volumes' parameter results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = "slaveId=" + slaveId.value();

  Future<Response> response =
    process::http::post(master.get()->pid, "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Create a volume so that a well-formed destroy attempt would succeed.
  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  response =
    process::http::post(master.get()->pid, "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test checks that dynamic reservations and persistent volumes
// can be created by frameworks using the offer cycle and then removed
// using the HTTP endpoints.
TEST_F(PersistentVolumeEndpointsTest, OfferCreateThenEndpointRemove)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(*):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Make a dynamic reservation for 512MB of disk.
  Resources unreserved = Resources::parse("disk:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_EQ(offer.slave_id(), slaveId);

  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)});

  // Make sure the allocator processes the `RESERVE` operation before summoning
  // an offer.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  // Create a 1MB persistent volume.
  Resources volume = createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
      "id1",
      "volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Create the volume.
  driver.acceptOffers({offer.id()}, {CREATE(volume)});

  // Make sure the master processes the accept call before summoning an offer.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_TRUE(metricEquals("master/operations/finished", 2));

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer with a persistent volume.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  // Destroy the volume using HTTP operator endpoint.
  Future<Response> destroyResponse = process::http::post(
      master.get()->pid,
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, destroyResponse);

  AWAIT_READY(rescindedOfferId);
  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer containing reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  // Unreserve the resources using HTTP operator endpoint.
  Future<Response> unreserveResponse = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, unreserveResponse);

  AWAIT_READY(rescindedOfferId);
  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer containing only unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test checks that dynamic reservations and persistent volumes
// can be created using the HTTP endpoints and then removed via the
// offer cycle.
TEST_F(PersistentVolumeEndpointsTest, EndpointCreateThenOfferRemove)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(*):1024";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  // Make a dynamic reservation for 512MB of disk.
  Resources unreserved = Resources::parse("disk:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus1 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  AWAIT_READY(acknowledgeOperationStatus1);

  // Create a 1MB persistent volume.
  Resources volume = createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
      "id1",
      "volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus2 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  AWAIT_READY(acknowledgeOperationStatus2);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Expect an offer containing the persistent volume.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus3 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  driver.acceptOffers({offer.id()}, {DESTROY(volume)});

  // Make sure the master processes the accept call before summoning an offer.
  AWAIT_READY(acknowledgeOperationStatus3);

  // We expect 3 finished operations, `Reserve`, `CreateVolume` and finally
  // `DestroyVolume`. The first two are not checked individually because it
  // would require additional heavy machinery to do it in a non-flaky way.
  EXPECT_TRUE(metricEquals("master/operations/finished", 3));

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer containing the dynamic reservation.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources())
                .contains(allocatedResources(
                    dynamicallyReserved, frameworkInfo.roles(0))))
    << Resources(offer.resources()) << " vs "
    << allocatedResources(dynamicallyReserved, frameworkInfo.roles(0));

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus4 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)});

  // Make sure the master processes the accept call before summoning an offer.
  AWAIT_READY(acknowledgeOperationStatus4);
  EXPECT_TRUE(metricEquals("master/operations/finished", 4));

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  // Expect an offer containing only unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test checks that a combination of HTTP endpoint reservations,
// framework reservations and unreservations, and slave removals works
// correctly. See MESOS-5698 for context.
TEST_F(PersistentVolumeEndpointsTest, ReserveAndSlaveRemoval)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slave1Flags = CreateSlaveFlags();
  slave1Flags.resources = "cpus:4";

  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector.get(), slave1Flags);
  ASSERT_SOME(slave1);

  Clock::advance(slave1Flags.registration_backoff_factor);
  AWAIT_READY(slave1RegisteredMessage);

  const SlaveID& slaveId1 = slave1RegisteredMessage->slave_id();

  // Capture the registration message for the second agent.
  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave1.get()->pid));

  // Each slave needs its own flags to ensure work_dirs are unique.
  slave::Flags slave2Flags = CreateSlaveFlags();
  slave2Flags.resources = "cpus:3";

  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get(), slave2Flags);
  ASSERT_SOME(slave2);

  Clock::advance(slave2Flags.registration_backoff_factor);
  AWAIT_READY(slave2RegisteredMessage);

  const SlaveID& slaveId2 = slave2RegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  // Reserve all CPUs on `slave1` via HTTP endpoint.
  Resources slave1Unreserved = Resources::parse("cpus:4").get();
  Resources slave1Reserved =
    slave1Unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus1 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId1, "resources", slave1Reserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  AWAIT_READY(acknowledgeOperationStatus1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(2u, offers->size());

  Future<ApplyOperationMessage> applyOperationMessage =
    FUTURE_PROTOBUF(ApplyOperationMessage(), _, _);

  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatus2 =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), _, _);

  // Use the offers API to reserve all CPUs on `slave2`.
  Resources slave2Unreserved = Resources::parse("cpus:3").get();
  Resources slave2Reserved =
    slave2Unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  for (size_t i = 0; i < offers->size(); i++) {
    const Offer& offer = offers.get()[i];
    const SlaveID& offeredSlaveId = offer.slave_id();

    ASSERT_TRUE(offeredSlaveId == slaveId1 ||
                offeredSlaveId == slaveId2);

    if (offeredSlaveId == slaveId2) {
      driver.acceptOffers({offer.id()}, {RESERVE(slave2Reserved)});
      break;
    }
  }

  Resources sentResources =
    applyOperationMessage->operation_info().reserve().resources();

  sentResources.unallocate();

  AWAIT_READY(applyOperationMessage);
  EXPECT_EQ(sentResources, slave2Reserved);

  AWAIT_READY(updateOperationStatusMessage);
  AWAIT_READY(acknowledgeOperationStatus2);

  // Expect both `Reserve` operations to be finished.
  EXPECT_TRUE(metricEquals("master/operations/finished", 2));

  // Shutdown `slave2` with an explicit shutdown message.
  EXPECT_CALL(sched, offerRescinded(_, _));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(_, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  slave2.get()->shutdown();

  AWAIT_READY(slaveLost);

  response = process::http::post(
      master.get()->pid,
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId1, "resources", slave1Reserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  driver.stop();
  driver.join();
}


// This tests that unreserved resources, dynamic reservations and persistent
// volumes are reflected in the "/slaves" master endpoint.
TEST_F(PersistentVolumeEndpointsTest, SlavesEndpointFullResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:4;gpus:0;mem:2048;disk:4096";

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("cpus:1;mem:512;disk:1024").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get()->pid,
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.roles(0),
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get()->pid,
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId, "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  // Start a framework and launch a task on some (but not all) of the
  // reserved resources at the slave.
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
      allocatedResources(volume, frameworkInfo.roles(0))));

  Resources taskUnreserved = Resources::parse("cpus:1;mem:256").get();
  Resources taskResources =
    taskUnreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  TaskInfo taskInfo = createTask(offer.slave_id(), taskResources, "sleep 1000");

  Future<TaskStatus> starting;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&starting))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(starting);
  EXPECT_EQ(TASK_STARTING, starting->state());

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  response = process::http::get(
      master.get()->pid,
      "slaves",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(parse);

  JSON::Object slavesObject = parse.get();

  ASSERT_TRUE(slavesObject.values["slaves"].is<JSON::Array>());
  JSON::Array slaveArray = slavesObject.values["slaves"].as<JSON::Array>();

  EXPECT_EQ(1u, slaveArray.values.size());

  ASSERT_TRUE(slaveArray.values[0].is<JSON::Object>());
  JSON::Object slaveObject = slaveArray.values[0].as<JSON::Object>();

  // TODO(greggomann): Use `DEFAULT_CREDENTIAL.principal()` instead of the
  // hard-coded principals below. See MESOS-5469.

  Try<JSON::Value> expectedReserved = JSON::parse(
      R"~(
      {
        "role1": [
          {
            "name": "cpus",
            "type": "SCALAR",
            "scalar": {
              "value": 1.0
            },
            "role": "role1",
            "reservation": {
              "principal": "test-principal"
            },
            "reservations":[
              {
                "principal": "test-principal",
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
              "principal": "test-principal"
            },
            "reservations": [
              {
                "principal": "test-principal",
                "role": "role1",
                "type": "DYNAMIC"
              }
            ]
          },
          {
            "name": "disk",
            "type": "SCALAR",
            "scalar": {
              "value": 960.0
            },
            "role": "role1",
            "reservation": {
              "principal": "test-principal"
            },
            "reservations": [
              {
                "principal": "test-principal",
                "role": "role1",
                "type": "DYNAMIC"
              }
            ]
          },
          {
            "name": "disk",
            "type": "SCALAR",
            "scalar": {
              "value": 64.0
            },
            "role": "role1",
            "reservation": {
              "principal": "test-principal"
            },
            "reservations": [
              {
                "principal": "test-principal",
                "role": "role1",
                "type": "DYNAMIC"
              }
            ],
            "disk": {
              "persistence": {
                "id": "id1",
                "principal": "test-principal"
              },
              "volume": {
                "mode": "RW",
                "container_path": "path1"
              }
            }
          }
        ]
      })~");

  ASSERT_SOME(expectedReserved);

  Try<JSON::Value> expectedUnreserved = JSON::parse(
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
      ])~");

  ASSERT_SOME(expectedUnreserved);

  Try<JSON::Value> expectedUsed = JSON::parse(
      R"~(
      [
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "cpus",
          "role": "role1",
          "reservation": {
            "principal": "test-principal"
          },
          "reservations": [
            {
              "principal": "test-principal",
              "role": "role1",
              "type": "DYNAMIC"
            }
          ],
          "scalar": {
            "value": 1.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "mem",
          "role": "role1",
          "reservation": {
            "principal": "test-principal"
          },
          "reservations": [
            {
              "principal": "test-principal",
              "role": "role1",
              "type": "DYNAMIC"
            }
          ],
          "scalar": {
            "value": 256.0
          },
          "type": "SCALAR"
        }
      ])~");

  ASSERT_SOME(expectedUsed);

  Try<JSON::Value> expectedOffered = JSON::parse(
      R"~(
      [
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "cpus",
          "role": "*",
          "scalar": {
            "value": 3.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "mem",
          "role": "*",
          "scalar": {
            "value": 1536.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "disk",
          "role": "*",
          "scalar": {
            "value": 3072.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
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
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "disk": {
            "persistence": {
              "id": "id1",
              "principal": "test-principal"
            },
            "volume": {
              "container_path": "path1",
              "mode": "RW"
            }
          },
          "name": "disk",
          "role": "role1",
          "reservation": {
            "principal": "test-principal"
          },
          "reservations": [
            {
              "principal": "test-principal",
              "role": "role1",
              "type": "DYNAMIC"
            }
          ],
          "scalar": {
            "value": 64.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "mem",
          "role": "role1",
          "reservation": {
            "principal": "test-principal"
          },
          "reservations": [
            {
              "principal": "test-principal",
              "role": "role1",
              "type": "DYNAMIC"
            }
          ],
          "scalar": {
            "value": 256.0
          },
          "type": "SCALAR"
        },
        {
          "allocation_info": {
            "role": "role1"
          },
          "name": "disk",
          "role": "role1",
          "reservation": {
            "principal": "test-principal"
          },
          "reservations": [
            {
              "principal": "test-principal",
              "role": "role1",
              "type": "DYNAMIC"
            }
          ],
          "scalar": {
            "value": 960.0
          },
          "type": "SCALAR"
        }
      ])~");

  ASSERT_SOME(expectedOffered);

  JSON::Value reservedValue = slaveObject.values["reserved_resources_full"];
  EXPECT_EQ(expectedReserved.get(), reservedValue);

  JSON::Value unreservedValue = slaveObject.values["unreserved_resources_full"];
  EXPECT_EQ(
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(expectedUnreserved->as<JSON::Array>()))),
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(unreservedValue.as<JSON::Array>()))));

  JSON::Value usedValue = slaveObject.values["used_resources_full"];
  EXPECT_EQ(
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(expectedUsed->as<JSON::Array>()))),
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(usedValue.as<JSON::Array>()))));

  JSON::Value offeredValue = slaveObject.values["offered_resources_full"];
  EXPECT_EQ(
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(expectedOffered->as<JSON::Array>()))),
      Resources(CHECK_NOTERROR(
          Resources::fromJSON(offeredValue.as<JSON::Array>()))));

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
