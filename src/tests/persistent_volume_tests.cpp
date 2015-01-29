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

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/hashset.hpp>
#include <stout/strings.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::tests;

using namespace process;

using mesos::master::Master;

using mesos::slave::Slave;

using std::string;
using std::vector;


class PersistentVolumeTest : public MesosTest
{
protected:
  master::Flags MasterFlags(const vector<FrameworkInfo>& frameworks)
  {
    master::Flags flags = CreateMasterFlags();

    ACLs acls;
    hashset<string> roles;

    foreach (const FrameworkInfo& framework, frameworks) {
      mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
      acl->mutable_principals()->add_values(framework.principal());
      acl->mutable_roles()->add_values(framework.role());

      roles.insert(framework.role());
    }

    flags.acls = acls;
    flags.roles = strings::join(",", roles);

    return flags;
  }

  Resource PersistentVolume(
      const Bytes& size,
      const string& role,
      const string& persistenceId,
      const string& containerPath)
  {
    Resource volume = Resources::parse(
        "disk",
        stringify(size.megabytes()),
        role).get();

    volume.mutable_disk()->CopyFrom(
        createDiskInfo(persistenceId, containerPath));

    return volume;
  }

  Offer::Operation CreateOperation(const Resources& volumes)
  {
    Offer::Operation operation;
    operation.set_type(Offer::Operation::CREATE);
    operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);
    return operation;
  }

  Offer::Operation DestroyOperation(const Resources& volumes)
  {
    Offer::Operation operation;
    operation.set_type(Offer::Operation::DESTROY);
    operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);
    return operation;
  }
};


// This test verifies that CheckpointResourcesMessages are sent to the
// slave when the framework creates/destroys persistent volumes, and
// the resources in the messages correctly reflect the resources that
// need to be checkpointed on the slave.
TEST_F(PersistentVolumeTest, SendingCheckpointResourcesMessage)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> message3 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Resources volume1 = PersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Resources volume2 = PersistentVolume(
      Megabytes(128),
      "role1",
      "id2",
      "path2");

  driver.acceptOffers(
      {offer.id()},
      {CreateOperation(volume1),
       CreateOperation(volume2),
       DestroyOperation(volume1)});

  // NOTE: Currently, we send one message per operation. But this is
  // an implementation detail which is subject to change.
  AWAIT_READY(message1);
  EXPECT_EQ(Resources(message1.get().resources()), volume1);

  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2.get().resources()), volume1 + volume2);

  AWAIT_READY(message3);
  EXPECT_EQ(Resources(message3.get().resources()), volume2);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the slave checkpoints the resources for
// persistent volumes to the disk, recovers them upon restart, and
// sends them to the master during re-registration.
TEST_F(PersistentVolumeTest, ResourcesCheckpointing)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.checkpoint = true;
  slaveFlags.resources = "disk(role1):1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  Resources volume = PersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  driver.acceptOffers(
      {offer.id()},
      {CreateOperation(volume)});

  AWAIT_READY(checkpointResources);

  // Restart the slave.
  Stop(slave.get());

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlave);
  EXPECT_EQ(Resources(reregisterSlave.get().checkpointed_resources()), volume);

  driver.stop();
  driver.join();

  Shutdown();
}
