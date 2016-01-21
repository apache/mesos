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

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using namespace process;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

enum PersistentVolumeSourceType
{
  NONE,
  PATH
};


class PersistentVolumeTest
  : public MesosTest,
    public WithParamInterface<PersistentVolumeSourceType>
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();
    Try<string> path = environment->mkdtemp();
    ASSERT_SOME(path) << "Failed to mkdtemp";
    diskPath = path.get();
  }

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

  Resource getDiskResource(const Megabytes& mb)
  {
    Resource diskResource;

    switch (GetParam()) {
      case NONE: {
        diskResource = createDiskResource(
            stringify(mb.megabytes()),
            "role1",
            None(),
            None());

        break;
      }
      case PATH: {
        diskResource = createDiskResource(
            stringify(mb.megabytes()),
            "role1",
            None(),
            None(),
            createDiskSourcePath(diskPath));

        break;
      }
    }

    return diskResource;
  }

  string getSlaveResources()
  {
    Resources resources = Resources::parse("cpus:2;mem:2048").get() +
      getDiskResource(Megabytes(2048));

    return stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(resources)));
  }

  string diskPath;
};


// The PersistentVolumeTest tests are parameterized by the disk source.
INSTANTIATE_TEST_CASE_P(
    DiskResource,
    PersistentVolumeTest,
    ::testing::Values(
        PersistentVolumeSourceType::NONE,
        PersistentVolumeSourceType::PATH));


// This test verifies that CheckpointResourcesMessages are sent to the
// slave when the framework creates/destroys persistent volumes, and
// the resources in the messages correctly reflect the resources that
// need to be checkpointed on the slave.
TEST_P(PersistentVolumeTest, SendingCheckpointResourcesMessage)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

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

  Resource volume1 = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  Resource volume2 = createPersistentVolume(
      getDiskResource(Megabytes(128)),
      "id2",
      "path2",
      None());

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create the persistent volumes via `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume1),
       CREATE(volume2)},
      filters);

  // Expect an offer containing the persistent volumes.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // NOTE: Currently, we send one message per operation. But this is
  // an implementation detail which is subject to change.
  AWAIT_READY(message1);
  EXPECT_EQ(Resources(message1.get().resources()), volume1);

  // Await the `CheckpointResourcesMessage` and ensure that it contains
  // both volume1 and volume2.
  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2.get().resources()),
            Resources(volume1) + Resources(volume2));

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Expect that the offer contains the persistent volumes we created.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume1));
  EXPECT_TRUE(Resources(offer.resources()).contains(volume2));

  // Destroy `volume1`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume1)},
      filters);

  // Await the `CheckpointResourcesMessage` and ensure that it contains
  // volume2 but not volume1.
  AWAIT_READY(message3);
  EXPECT_EQ(Resources(message3.get().resources()), volume2);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the slave checkpoints the resources for
// persistent volumes to the disk, recovers them upon restart, and
// sends them to the master during re-registration.
TEST_P(PersistentVolumeTest, ResourcesCheckpointing)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

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

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

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


TEST_P(PersistentVolumeTest, PreparePersistentVolume)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

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

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  // This is to make sure CheckpointResourcesMessage is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies the case where a slave that has checkpointed
// persistent volumes reregisters with a failed over master, and the
// persistent volumes are later correctly offered to the framework.
TEST_P(PersistentVolumeTest, MasterFailover)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  master::Flags masterFlags = MasterFlags({frameworkInfo});

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_FALSE(offers1.get().empty());

  Offer offer1 = offers1.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  // This is to make sure CheckpointResourcesMessage is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Simulate failed over master by restarting the master.
  Stop(master.get());

  EXPECT_CALL(sched, disconnected(&driver));

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master.get());

  AWAIT_READY(slaveReregistered);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2.get().empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_TRUE(Resources(offer2.resources()).contains(volume));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that a slave will refuse to start if the
// checkpointed resources it recovers are not compatible with the
// slave resources specified using the '--resources' flag.
TEST_P(PersistentVolumeTest, IncompatibleCheckpointedResources)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get());

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

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

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  terminate(slave1);
  wait(slave1);

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it's not compatible with the
  // checkpointed resources.
  slaveFlags.resources = "disk:1024";

  MockSlave slave2(slaveFlags, &detector, &containerizer);

  Future<Future<Nothing>> recover;
  EXPECT_CALL(slave2, __recover(_))
    .WillOnce(DoAll(FutureArg<0>(&recover), Return()));

  spawn(slave2);

  AWAIT_READY(recover);
  AWAIT_FAILED(recover.get());

  terminate(slave2);
  wait(slave2);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that a persistent volume is correctly linked by
// the containerizer and the task is able to access it according to
// the container path it specifies.
TEST_P(PersistentVolumeTest, AccessPersistentVolume)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  // Create a task which writes a file in the persistent volume.
  Resources taskResources = Resources::parse("cpus:1;mem:128").get() +
    getDiskResource(Megabytes(32)) + volume;

  TaskInfo task = createTask(
      offer.slave_id(),
      taskResources,
      "echo abc > path1/file");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume),
       LAUNCH({task})});

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2.get().task_id());
  EXPECT_EQ(TASK_FINISHED, status2.get().state());

  // This is to verify that the persistent volume is correctly
  // unlinked from the executor working directory after TASK_FINISHED
  // is received by the scheduler (at which time the container's
  // resources should already be updated).

  // NOTE: The command executor's id is the same as the task id.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  const string& directory = slave::paths::getExecutorLatestRunPath(
      slaveFlags.work_dir,
      offer.slave_id(),
      frameworkId.get(),
      executorId);

  EXPECT_FALSE(os::exists(path::join(directory, "path1")));

  const string& volumePath = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume);

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volumePath, "file")));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that persistent volumes are recovered properly
// after the slave restarts. The idea is to launch a command which
// keeps testing if the persistent volume exists, and fails if it does
// not. So the framework should not receive a TASK_FAILED after the
// slave finishes recovery.
TEST_P(PersistentVolumeTest, SlaveRecovery)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(64)),
      "id1",
      "path1",
      None());

  // Create a task which writes a file in the persistent volume.
  Resources taskResources = Resources::parse("cpus:1;mem:128").get() +
    getDiskResource(Megabytes(32)) + volume;

  TaskInfo task = createTask(
      offer.slave_id(),
      taskResources,
      "while true; do test -d path1; done");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})});

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Restart the slave.
  Stop(slave.get());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(reregisterExecutorMessage);

  // Wait for slave to schedule reregister timeout.
  Clock::settle();

  // Ensure the slave considers itself recovered.
  Clock::advance(slave::EXECUTOR_REREGISTER_TIMEOUT);

  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  // The framework should not receive a TASK_FAILED here since the
  // persistent volume shouldn't be affected even if slave restarts.
  ASSERT_TRUE(status2.isPending());

  // NOTE: We kill the task and wait for TASK_KILLED here to make sure
  // any pending status updates are received by the framework.
  driver.killTask(task.task_id());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2.get().task_id());
  EXPECT_EQ(TASK_KILLED, status2.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the `create` and `destroy` operations complete
// successfully when authorization succeeds.
TEST_P(PersistentVolumeTest, GoodACLCreateThenDestroy)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This ACL declares that the principal of `DEFAULT_CREDENTIAL`
  // can create any persistent volumes.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that the principal of `DEFAULT_CREDENTIAL`
  // can destroy its own persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  destroy->mutable_creator_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.role();

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Expect an offer from the slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(128)),
      "id1",
      "path1",
      None());

  Future<CheckpointResourcesMessage> checkpointResources1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the CheckpointResourceMessage response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(checkpointResources1);
  EXPECT_EQ(Resources(checkpointResources1.get().resources()), volume);

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume was created successfully.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<CheckpointResourcesMessage> checkpointResources2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // Destroy the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  AWAIT_READY(checkpointResources2);
  EXPECT_FALSE(
      Resources(checkpointResources2.get().resources()).contains(volume));

  // Expect an offer that does not contain the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume is not in the offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(volume));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the `create` and `destroy` operations complete
// successfully when authorization succeeds and no principal is provided.
TEST_P(PersistentVolumeTest, GoodACLNoPrincipal)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This ACL declares that any principal (and also frameworks without a
  // principal) can create persistent volumes.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that any principal (and also frameworks without a
  // principal) can destroy persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  destroy->mutable_creator_principals()->set_type(mesos::ACL::Entity::ANY);

  // We use the filter explicitly here so that the resources will not be
  // filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` with no principal.
  FrameworkInfo frameworkInfo;
  frameworkInfo.set_name("no-principal");
  frameworkInfo.set_user(os::user().get());
  frameworkInfo.set_role("role1");

  // Create a master. Since the framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.role();
  masterFlags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Expect an offer from the slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(128)),
      "id1",
      "path1",
      None());

  Future<CheckpointResourcesMessage> checkpointResources1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the CheckpointResourceMessage response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(checkpointResources1);
  EXPECT_EQ(Resources(checkpointResources1.get().resources()), volume);

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume was successfully created.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<CheckpointResourcesMessage> checkpointResources2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // Destroy the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  AWAIT_READY(checkpointResources2);

  // Expect an offer that does not contain the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume was not created
  EXPECT_FALSE(Resources(offer.resources()).contains(volume));
  EXPECT_FALSE(
      Resources(checkpointResources2.get().resources()).contains(volume));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that `create` and `destroy` operations fail as expected
// when authorization fails and no principal is supplied.
TEST_P(PersistentVolumeTest, BadACLNoPrincipal)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This ACL declares that the principal of `DEFAULT_FRAMEWORK_INFO`
  // can create persistent volumes.
  mesos::ACL::CreateVolume* create1 = acls.add_create_volumes();
  create1->mutable_principals()->add_values(DEFAULT_FRAMEWORK_INFO.principal());
  create1->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that any other principals
  // cannot create persistent volumes.
  mesos::ACL::CreateVolume* create2 = acls.add_create_volumes();
  create2->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create2->mutable_volume_types()->set_type(mesos::ACL::Entity::NONE);

  // We use this filter so that resources will not
  // be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` with no principal.
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("no-principal");
  frameworkInfo1.set_user(os::user().get());
  frameworkInfo1.set_role("role1");

  // Create a `FrameworkInfo` with a principal.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_role("role1");

  // Create a master. Since one framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.role();
  masterFlags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get());

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // Expect an offer from the slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(128)),
      "id1",
      "path1",
      None());

  // Attempt to create the persistent volume using `acceptOffers`.
  driver1.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume is not contained in this offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(volume));

  // Decline the offer and suppress so the second
  // framework will receive the offer instead.
  driver1.declineOffer(offer.id(), filters);
  driver1.suppressOffers();

  // Create a second framework which can create volumes.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Expect an offer to the second framework.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Create the persistent volume using `acceptOffers`.
  driver2.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  // Decline and suppress offers to `driver2` so that
  // `driver1` can receive an offer.
  driver2.declineOffer(offer.id(), filters);
  driver2.suppressOffers();

  // Settle the clock to ensure that `driver2`
  // suppresses before `driver1` revives.
  Clock::settle();

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // Revive offers to `driver1`. Settling and advancing the clock after this is
  // unnecessary, since calling `reviveOffers` triggers an offer.
  driver1.reviveOffers();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Attempt to destroy the persistent volume using `acceptOffers`.
  driver1.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  // Check that the persistent volume is still contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
}


// This test verifies that `create` and `destroy` operations
// get dropped if authorization fails.
TEST_P(PersistentVolumeTest, BadACLDropCreateAndDestroy)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This ACL declares that the principal 'creator-principal'
  // can create persistent volumes.
  mesos::ACL::CreateVolume* create1 = acls.add_create_volumes();
  create1->mutable_principals()->add_values("creator-principal");
  create1->mutable_volume_types()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that all other principals
  // cannot create any persistent volumes.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create->mutable_volume_types()->set_type(mesos::ACL::Entity::NONE);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` that cannot create or destroy volumes.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_role("role1");

  // Create a `FrameworkInfo` that can create volumes.
  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("creator-framework");
  frameworkInfo2.set_user(os::user().get());
  frameworkInfo2.set_role("role1");
  frameworkInfo2.set_principal("creator-principal");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.role();
  masterFlags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get());

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // Expect an offer from the slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(128)),
      "id1",
      "path1",
      None());

  // Attempt to create a persistent volume using `acceptOffers`.
  driver1.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume is not contained in this offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(volume));

  // Decline the offer and suppress so the second
  // framework will receive the offer instead.
  driver1.declineOffer(offer.id(), filters);
  driver1.suppressOffers();

  // Create a second framework which can create volumes.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Expect an offer to the second framework.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Create a persistent volume using `acceptOffers`.
  driver2.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  // Decline and suppress offers to `driver2` so that
  // `driver1` can receive an offer.
  driver2.declineOffer(offer.id(), filters);
  driver2.suppressOffers();

  // Settle the clock to ensure that `driver2`
  // suppresses before `driver1` revives.
  Clock::settle();

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // Revive offers to `driver1`. Settling and advancing the clock after this is
  // unnecessary, since calling `reviveOffers` triggers an offer.
  driver1.reviveOffers();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  offer = offers.get()[0];

  // Attempt to destroy the persistent volume using `acceptOffers`.
  driver1.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  // Expect another offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  // Check that the persistent volume is still contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
