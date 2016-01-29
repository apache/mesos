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

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::DEFAULT_ALLOCATION_INTERVAL;
using mesos::internal::master::Master;
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
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class PersistentVolumeEndpointsTest : public MesosTest
{
public:
  // Set up the master flags such that it allows registration of the
  // framework created with 'createFrameworkInfo'.
  virtual master::Flags CreateMasterFlags()
  {
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Milliseconds(50);
    flags.roles = createFrameworkInfo().role();
    return flags;
  }

  // Returns a FrameworkInfo with role, "role1".
  FrameworkInfo createFrameworkInfo()
  {
    FrameworkInfo info = DEFAULT_FRAMEWORK_INFO;
    info.set_role("role1");
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
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

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

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Response> destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, destroyResponse);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_FALSE(Resources(offer.resources()).contains(volume));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an operator can create a persistent volume from
// dynamically reserved resources, and can then destroy that volume.
TEST_F(PersistentVolumeEndpointsTest, DynamicReservation)
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
  slaveFlags.resources = "disk(*):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Offer the dynamically reserved resources to a framework. The
  // offer should be rescinded when a persistent volume is created
  // using the same resources (below).
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

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.role(),
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  // After destroying the volume, we should rescind the previous offer
  // containing the volume.
  response = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an attempt to create a persistent volume fails with
// a 'Conflict' HTTP error if the only available reserved resources on
// the slave have been reserved by a different role.
TEST_F(PersistentVolumeEndpointsTest, DynamicReservationRoleMismatch)
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
  slaveFlags.resources = "disk(*):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "resources", dynamicallyReserved));

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

  ASSERT_NE(frameworkInfo.role(), "role2");
  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role2",
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an attempt to unreserve the resources used by a
// persistent volume results in a 'Conflict' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, UnreserveVolumeResources)
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
  slaveFlags.resources = "disk(*):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

  Future<Response> response = process::http::post(
      master.get(),
      "reserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.role(),
      "id1",
      "path1",
      DEFAULT_CREDENTIAL.principal());

  response = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response = process::http::post(
      master.get(),
      "unreserve",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "resources", dynamicallyReserved));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response);

  Shutdown();
}


// This tests that an attempt to create a volume that is larger than the
// reserved resources at the slave results in a 'Conflict' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, VolumeExceedsReservedSize)
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
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(1025),
      "role1",
      "id1",
      "path1");

  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, createResponse);

  Shutdown();
}


// This tests that an attempt to delete a non-existent persistent
// volume results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, DeleteNonExistentVolume)
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
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, createResponse);

  // Non-existent volume ID.
  Resources badVolumeId = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id2",
      "path1");

  Future<Response> destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", badVolumeId));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // Non-existent role.
  Resources badRole = createPersistentVolume(
      Megabytes(64),
      "role2",
      "id1",
      "path1");

  destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", badRole));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // Size mismatch.
  Resources badSize = createPersistentVolume(
      Megabytes(128),
      "role1",
      "id1",
      "path1");

  destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", badSize));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, destroyResponse);

  // NOTE: Two persistent volumes with different paths are considered
  // equivalent, so the destroy operation will succeed. It is unclear
  // whether this behavior is desirable (MESOS-3961).
  Resources differentPath = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path2");

  destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", differentPath));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, destroyResponse);

  Shutdown();
}


// This tests that an attempt to create or destroy a volume with no
// authorization header results in an 'Unauthorized' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoHeader)
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
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.role(),
      "id1",
      "path1");

  Future<Response> response = process::http::post(
      master.get(),
      "create-volumes",
      None(),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  response = process::http::post(
      master.get(),
      "destroy-volumes",
      None(),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  Shutdown();
}


// This tests that an attempt to create or destroy a volume with bad
// credentials results in an 'Unauthorized' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, BadCredentials)
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
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Credential credential;
  credential.set_principal("bad-principal");
  credential.set_secret("bad-secret");

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  process::http::Headers headers = createBasicAuthHeaders(credential);
  string body = createRequestBody(slaveId.get(), "volumes", volume);

  Future<Response> response =
    process::http::post(master.get(), "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  response =
    process::http::post(master.get(), "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      response);

  Shutdown();
}


// This tests that correct setup of CreateVolume/DestroyVolume ACLs allows an
// operator to perform volume creation/destruction operations successfully.
TEST_F(PersistentVolumeEndpointsTest, GoodCreateAndDestroyACL)
{
  // Pause the clock to gain control over the offer cycle.
  Clock::pause();

  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create ANY volumes.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can destroy volumes that it created.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  destroy->mutable_creator_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator), FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::settle();
  Clock::advance(DEFAULT_ALLOCATION_INTERVAL);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Response> destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, destroyResponse);

  Clock::settle();
  Clock::advance(DEFAULT_ALLOCATION_INTERVAL);

  AWAIT_READY(rescindedOfferId);

  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_FALSE(Resources(offer.resources()).contains(volume));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an ACL prohibiting the creation of a persistent volume by a
// principal will lead to a properly failed request.
TEST_F(PersistentVolumeEndpointsTest, BadCreateAndDestroyACL)
{
  // Pause the clock to gain control over the offer cycle.
  Clock::pause();

  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // cannot create persistent volumes.
  mesos::ACL::CreateVolume* cannotCreate = acls.add_create_volumes();
  cannotCreate->mutable_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());
  cannotCreate->mutable_volume_types()->set_type(mesos::ACL::Entity::NONE);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL_2`
  // can create persistent volumes.
  mesos::ACL::CreateVolume* canCreate = acls.add_create_volumes();
  canCreate->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
  canCreate->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // cannot destroy persistent volumes.
  mesos::ACL::DestroyVolume* cannotDestroy = acls.add_destroy_volumes();
  cannotDestroy->mutable_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());
  cannotDestroy->mutable_creator_principals()->set_type(
      mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // The failed creation attempt.
  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, createResponse);

  // The successful creation attempt.
  createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::settle();
  Clock::advance(DEFAULT_ALLOCATION_INTERVAL);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  // The failed destruction attempt.
  Future<Response> destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, destroyResponse);

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that a request containing a credential which is not listed in the
// master for authentication will not succeed, even if a good authorization ACL
// is provided.
TEST_F(PersistentVolumeEndpointsTest, GoodCreateAndDestroyACLBadCredential)
{
  // Pause the clock to gain control over the offer cycle.
  Clock::pause();

  // Create a credential which will not be listed
  // for valid authentication with the master.
  Credential failedCredential;
  failedCredential.set_principal("awesome-principal");
  failedCredential.set_secret("super-secret-secret");

  TestAllocator<> allocator;
  ACLs acls;

  // This ACL asserts that the principal of `failedCredential`
  // can create persistent volumes.
  mesos::ACL::CreateVolume* failedCreate = acls.add_create_volumes();
  failedCreate->mutable_principals()->add_values(failedCredential.principal());
  failedCreate->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `failedCredential`
  // can destroy persistent volumes.
  mesos::ACL::DestroyVolume* failedDestroy = acls.add_destroy_volumes();
  failedDestroy->mutable_principals()->add_values(failedCredential.principal());
  failedDestroy->mutable_creator_principals()->set_type(
      mesos::ACL::Entity::ANY);

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL`
  // can create persistent volumes.
  mesos::ACL::CreateVolume* canCreate = acls.add_create_volumes();
  canCreate->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  canCreate->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Disk resources are statically reserved to allow the
  // creation of a persistent volume.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk(role1):1024";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // The failed creation attempt.
  Future<Response> createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(failedCredential),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      createResponse);

  // The successful creation attempt.
  createResponse = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, createResponse);

  FrameworkInfo frameworkInfo = createFrameworkInfo();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::settle();
  Clock::advance(DEFAULT_ALLOCATION_INTERVAL);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  // The failed destruction attempt.
  Future<Response> destroyResponse = process::http::post(
      master.get(),
      "destroy-volumes",
      createBasicAuthHeaders(failedCredential),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized({}).status,
      destroyResponse);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test creates and destroys a volume without authentication headers and
// with authorization disabled. These requests will succeed because HTTP
// authentication is disabled, so authentication headers are not required.
// Additionally, since authorization is not enabled, any principal, including
// `None()`, can create and destroy volumes.
TEST_F(PersistentVolumeEndpointsTest, NoAuthentication)
{
  const string TEST_ROLE = "role1";

  TestAllocator<> allocator;

  // Create master flags that will disable authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http = false;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Create an agent with statically reserved disk resources to allow the
  // creation of a persistent volume.
  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:1;mem:512;disk(" + TEST_ROLE + "):1024";

  Future<SlaveID> agentId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator), FutureArg<0>(&agentId)));

  Try<PID<Slave>> agent = StartSlave(agentFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(agentId);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      TEST_ROLE,
      "id1",
      "path1");

  // Make a request to create a volume with no authentication header.
  {
    Future<Response> response = process::http::post(
        master.get(),
        "create-volumes",
        None(),
        createRequestBody(agentId.get(), "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // Make a request to destroy a volume with no authentication header.
  {
    Future<Response> response = process::http::post(
        master.get(),
        "destroy-volumes",
        None(),
        createRequestBody(agentId.get(), "volumes", volume));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  Shutdown();
}


// This tests that an attempt to create or destroy a volume with no
// 'slaveId' results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoSlaveId)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body =
    "volumes=" +
    stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(volume)));

  Future<Response> response =
    process::http::post(master.get(), "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Create a volume so that a well-formed destroy attempt would succeed.
  response = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response =
    process::http::post(master.get(), "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This tests that an attempt to create or destroy a volume without
// the 'volumes' parameter results in a 'BadRequest' HTTP error.
TEST_F(PersistentVolumeEndpointsTest, NoVolumes)
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
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  string body = "slaveId=" + slaveId.get().value();

  Future<Response> response =
    process::http::post(master.get(), "create-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Create a volume so that a well-formed destroy attempt would succeed.
  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  response = process::http::post(
      master.get(),
      "create-volumes",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(slaveId.get(), "volumes", volume));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response =
    process::http::post(master.get(), "destroy-volumes", headers, body);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
