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

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using std::string;
using std::vector;

using testing::_;
using testing::Return;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

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

  Resources volume1 = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Resources volume2 = createPersistentVolume(
      Megabytes(128),
      "role1",
      "id2",
      "path2");

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume1),
       CREATE(volume2),
       DESTROY(volume1)});

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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

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


TEST_F(PersistentVolumeTest, PreparePersistentVolume)
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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

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

  EXPECT_TRUE(
      os::exists(slave::paths::getPersistentVolumePath(
          slaveFlags.work_dir,
          "role1",
          "id1")));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies the case where a slave that has checkpointed
// persistent volumes reregisters with a failed over master, and the
// persistent volumes are later correctly offered to the framework.
TEST_F(PersistentVolumeTest, MasterFailover)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  master::Flags masterFlags = MasterFlags({frameworkInfo});

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

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
TEST_F(PersistentVolumeTest, IncompatibleCheckpointedResources)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  terminate(slave1);
  wait(slave1);

  // Simulate a reboot of the slave machine by modify the boot ID.
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
TEST_F(PersistentVolumeTest, AccessPersistentVolume)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = "cpus:2;mem:1024;disk(role1):1024";

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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // Create a task which writes a file in the persistent volume.
  Resources taskResources =
    Resources::parse("cpus:1;mem:128;disk(role1):32").get() + volume;

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
      "role1",
      "id1");

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
TEST_F(PersistentVolumeTest, SlaveRecovery)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  Try<PID<Master>> master = StartMaster(MasterFlags({frameworkInfo}));
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = "cpus:2;mem:1024;disk(role1):1024";

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

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // Create a task which writes a file in the persistent volume.
  Resources taskResources =
    Resources::parse("cpus:1;mem:128;disk(role1):32").get() + volume;

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

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
