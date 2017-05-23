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

#include <list>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/none.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"

using namespace process;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using std::list;
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
  PATH,
  MOUNT
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

    if (GetParam() == MOUNT) {
      // On linux we mount a `tmpfs`.
#ifdef __linux__
      for (size_t i = 1; i <= NUM_DISKS; ++i) {
        string disk = path::join(diskPath, "disk" + stringify(i));
        ASSERT_SOME(os::mkdir(disk));
        ASSERT_SOME(fs::mount(None(), disk, "tmpfs", 0, "size=10M"));
      }
#else // __linux__
    // Otherwise we need to create 2 directories to mock the 2 devices.
      for (size_t i = 1; i <= NUM_DISKS; ++i) {
        string disk = path::join(diskPath, "disk" + stringify(i));
        ASSERT_SOME(os::mkdir(disk));
      }
#endif // __linux__
    }
  }

  virtual void TearDown()
  {
#ifdef __linux__
    if (GetParam() == MOUNT) {
      for (size_t i = 1; i <= NUM_DISKS; ++i) {
        ASSERT_SOME(
            fs::unmountAll(path::join(diskPath, "disk" + stringify(i))));
      }
    }
#endif // __linux__
    MesosTest::TearDown();
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

  // Creates a disk with / without a `source` based on the
  // parameterization of the test. `id` influences the `root` if one
  // is specified so that we can create multiple disks in the tests.
  Resource getDiskResource(const Megabytes& mb, size_t id = 1)
  {
    CHECK_LE(1u, id);
    CHECK_GE(NUM_DISKS, id);
    Resource diskResource;

    switch (GetParam()) {
      case NONE: {
        diskResource = createDiskResource(
            stringify(mb.megabytes()),
            DEFAULT_TEST_ROLE,
            None(),
            None());

        break;
      }
      case PATH: {
        diskResource = createDiskResource(
            stringify(mb.megabytes()),
            DEFAULT_TEST_ROLE,
            None(),
            None(),
            createDiskSourcePath(path::join(diskPath, "disk" + stringify(id))));

        break;
      }
      case MOUNT: {
        diskResource = createDiskResource(
            stringify(mb.megabytes()),
            DEFAULT_TEST_ROLE,
            None(),
            None(),
            createDiskSourceMount(
                path::join(diskPath, "disk" + stringify(id))));

        break;
      }
    }

    return diskResource;
  }

  string getSlaveResources()
  {
    // Create 2 disks that can be used to create persistent volumes.
    // NOTE: These will be merged if our fixture parameter is `NONE`.
    Resources resources = Resources::parse("cpus:2;mem:2048").get() +
      getDiskResource(Megabytes(2048), 1) +
      getDiskResource(Megabytes(2048), 2);

    return stringify(JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(resources)));
  }

  static constexpr size_t NUM_DISKS = 2;
  string diskPath;
};


// The PersistentVolumeTest tests are parameterized by the disk source.
INSTANTIATE_TEST_CASE_P(
    DiskResource,
    PersistentVolumeTest,
    ::testing::Values(
        PersistentVolumeSourceType::NONE,
        PersistentVolumeSourceType::PATH));


// We also want to parameterize them for `MOUNT`. On linux this means
// using `tmpfs`.
#ifdef __linux__
// On linux we have to run this test as root, as we need permissions
// to access `tmpfs`.
INSTANTIATE_TEST_CASE_P(
    ROOT_MountDiskResource,
    PersistentVolumeTest,
    ::testing::Values(
        PersistentVolumeSourceType::MOUNT));
#else // __linux__
// Otherwise we can run it without root privileges as we just require
// a directory.
INSTANTIATE_TEST_CASE_P(
    MountDiskResource,
    PersistentVolumeTest,
    ::testing::Values(
        PersistentVolumeSourceType::MOUNT));
#endif // __linux__


// This test verifies that CheckpointResourcesMessages are sent to the
// slave when the framework creates/destroys persistent volumes, and
// the resources in the messages correctly reflect the resources that
// need to be checkpointed on the slave.
TEST_P(PersistentVolumeTest, CreateAndDestroyPersistentVolumes)
{
  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> message3 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Resource volume1 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Resource volume2 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 2),
      "id2",
      "path2",
      None(),
      frameworkInfo.principal());

  string volume1Path = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir, volume1);

  string volume2Path = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir, volume2);

  // For MOUNT disks, we expect the volume's directory to already
  // exist (it is created by the test `SetUp()` function). For
  // non-MOUNT disks, the directory is created when the persistent
  // volume is created.
  if (GetParam() == MOUNT) {
    EXPECT_TRUE(os::exists(volume1Path));
    EXPECT_TRUE(os::exists(volume2Path));
  } else {
    EXPECT_FALSE(os::exists(volume1Path));
    EXPECT_FALSE(os::exists(volume2Path));
  }

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
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
  EXPECT_EQ(Resources(message1->resources()), volume1);

  // Await the `CheckpointResourcesMessage` and ensure that it contains
  // both volume1 and volume2.
  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2->resources()),
            Resources(volume1) + Resources(volume2));

  // Ensure that the `CheckpointResourcesMessage`s reach the slave.
  Clock::settle();

  EXPECT_TRUE(os::exists(volume1Path));
  EXPECT_TRUE(os::exists(volume2Path));

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Expect that the offer contains the persistent volumes we created.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume1, frameworkInfo.role())));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume2, frameworkInfo.role())));

  // Destroy `volume1`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume1)},
      filters);

  // Await the `CheckpointResourcesMessage` and ensure that it contains
  // volume2 but not volume1.
  AWAIT_READY(message3);
  EXPECT_EQ(Resources(message3->resources()), volume2);

  // Ensure that the `CheckpointResourcesMessage`s reach the slave.
  Clock::settle();

  // For MOUNT disks, we preserve the top-level volume directory (but
  // delete all of the files and subdirectories underneath it). For
  // non-MOUNT disks, the volume directory should be removed when the
  // volume is destroyed.
  if (GetParam() == MOUNT) {
    EXPECT_TRUE(os::exists(volume1Path));

    Try<list<string>> files = ::fs::list(path::join(volume1Path, "*"));
    ASSERT_SOME(files);
    EXPECT_EQ(0u, files->size());
  } else {
    EXPECT_FALSE(os::exists(volume1Path));
  }

  EXPECT_TRUE(os::exists(volume2Path));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the slave checkpoints the resources for
// persistent volumes to the disk, recovers them upon restart, and
// sends them to the master during re-registration.
TEST_P(PersistentVolumeTest, ResourcesCheckpointing)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  // Restart the slave.
  slave.get()->terminate();

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlave);
  EXPECT_EQ(Resources(reregisterSlave->checkpointed_resources()), volume);

  driver.stop();
  driver.join();
}


TEST_P(PersistentVolumeTest, PreparePersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

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
}


// This test verifies the case where a slave that has checkpointed
// persistent volumes reregisters with a failed over master, and the
// persistent volumes are later correctly offered to the framework.
TEST_P(PersistentVolumeTest, MasterFailover)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume)});

  AWAIT_READY(checkpointResources);

  // This is to make sure CheckpointResourcesMessage is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate failed over master by restarting the master.
  master->reset();

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master.get()->pid);

  // Ensure that agent re-registration occurs.
  Clock::pause();
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(slaveReregistered);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that a slave will refuse to start if the
// checkpointed resources it recovers are not compatible with the
// slave resources specified using the '--resources' flag.
TEST_P(PersistentVolumeTest, IncompatibleCheckpointedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

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
}


// This test verifies that a persistent volume is correctly linked by
// the containerizer and the task is able to access it according to
// the container path it specifies.
TEST_P(PersistentVolumeTest, AccessPersistentVolume)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  // Create a task which writes a file in the persistent volume.
  Resources taskResources = Resources::parse("cpus:1;mem:128").get() + volume;

  TaskInfo task = createTask(
      offer.slave_id(),
      taskResources,
      "echo abc > path1/file");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Future<Nothing> statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume),
       LAUNCH({task})});

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_FINISHED, status2->state());

  // This is to verify that the persistent volume is correctly
  // unlinked from the executor working directory after TASK_FINISHED
  // is received by the scheduler (at which time the container's
  // resources should already be updated).

  // NOTE: The command executor's id is the same as the task id.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  string directory = slave::paths::getExecutorLatestRunPath(
      slaveFlags.work_dir,
      offer.slave_id(),
      frameworkId.get(),
      executorId);

  EXPECT_FALSE(os::exists(path::join(directory, "path1")));

  string volumePath = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume);

  string filePath1 = path::join(volumePath, "file");

  EXPECT_SOME_EQ("abc\n", os::read(filePath1));

  // Ensure that the slave has received the acknowledgment of the
  // TASK_FINISHED status update; this implies the acknowledgement
  // reached the master, which is necessary for the task's resources
  // to be recovered by the allocator.
  AWAIT_READY(statusUpdateAcknowledgement1);
  AWAIT_READY(statusUpdateAcknowledgement2);

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);

  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> checkpointMessage =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  driver.acceptOffers({offer.id()}, {DESTROY(volume)});

  AWAIT_READY(checkpointMessage);

  Resources checkpointResources = checkpointMessage->resources();
  EXPECT_FALSE(checkpointResources.contains(volume));

  // Ensure that `checkpointMessage` reaches the slave.
  Clock::settle();

  EXPECT_FALSE(os::exists(filePath1));

  // For MOUNT disks, we preserve the top-level volume directory (but
  // delete all of the files and subdirectories underneath it). For
  // non-MOUNT disks, the volume directory should be removed when the
  // volume is destroyed.
  if (GetParam() == MOUNT) {
    EXPECT_TRUE(os::exists(volumePath));

    Try<list<string>> files = ::fs::list(path::join(volumePath, "*"));
    CHECK_SOME(files);
    EXPECT_EQ(0u, files->size());
  } else {
    EXPECT_FALSE(os::exists(volumePath));
  }

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that multiple tasks can be launched on a shared
// persistent volume and write to it simultaneously.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeMultipleTasks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources =
    "cpus:2;mem:1024;disk(" + string(DEFAULT_TEST_ROLE) + "):1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resources volume = createPersistentVolume(
      Megabytes(64),
      DEFAULT_TEST_ROLE,
      "id1",
      "path1",
      None(),
      None(),
      frameworkInfo.principal(),
      true); // Shared.

  // Create 2 tasks which write distinct files in the shared volume.
  Try<Resources> taskResources1 = Resources::parse(
      "cpus:1;mem:128;disk(" + string(DEFAULT_TEST_ROLE) + "):32");

  TaskInfo task1 = createTask(
      offer.slave_id(),
      taskResources1.get() + volume,
      "echo task1 > path1/file1");

  Try<Resources> taskResources2 = Resources::parse(
      "cpus:1;mem:256;disk(" + string(DEFAULT_TEST_ROLE) + "):64");

  TaskInfo task2 = createTask(
      offer.slave_id(),
      taskResources2.get() + volume,
      "echo task2 > path1/file2");

  // We should receive a TASK_RUNNING followed by a TASK_FINISHED for
  // each of the 2 tasks. We do not check for the actual task state
  // since it's not the primary objective of the test. We instead
  // verify that the paths are created by the tasks after we receive
  // enough status updates.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume),
       LAUNCH({task1, task2})});

  // When all status updates are received, the tasks have finished
  // writing to the paths.
  AWAIT_READY(status1);
  AWAIT_READY(status2);
  AWAIT_READY(status3);
  AWAIT_READY(status4);

  const string& volumePath = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      DEFAULT_TEST_ROLE,
      "id1");

  EXPECT_SOME_EQ("task1\n", os::read(path::join(volumePath, "file1")));
  EXPECT_SOME_EQ("task2\n", os::read(path::join(volumePath, "file2")));

  driver.stop();
  driver.join();
}


// This test verifies that pending offers with shared persistent volumes
// are rescinded when the volumes are destroyed.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeRescindOnDestroy)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // 1. Create framework1 so that all resources are offered to this framework.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo1.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  EXPECT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  // 2. framework1 CREATEs 2 shared volumes, and LAUNCHes a task with a subset
  //    of resources from the offer.
  Resource volume1 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo1.principal(),
      true); // Shared volume.

  Resource volume2 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 2),
      "id2",
      "path2",
      None(),
      frameworkInfo1.principal(),
      true); // Shared volume.

  Resources allVolumes;
  allVolumes += volume1;
  allVolumes += volume2;

  // Create a task which uses a portion of the offered resources, so that
  // the remaining resources can be offered to framework2. It's not important
  // whether the volume is used (the task is killed soon and its purpose is
  // only for splitting the offer).
  TaskInfo task = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "sleep 1000");

  // Expect an offer containing the persistent volumes.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // We use a filter of 0 seconds so the resources will be available
  // in the next allocation cycle.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver1.acceptOffers(
      {offer1.id()},
      {CREATE(allVolumes),
       LAUNCH({task})},
      filters);

  // Make sure the call is processed before framework2 registers.
  Clock::settle();

  // 3. Create framework2 of the same role. It would be offered resources
  //    recovered from the framework1 call.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo2.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver2.start();

  AWAIT_READY(offers2);

  Offer offer2 = offers2.get()[0];

  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume1, frameworkInfo2.role())));
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume2, frameworkInfo2.role())));

  // 4. framework1 kills the task which results in an offer to framework1
  //    with the shared volumes. At this point, both frameworks will have
  //    the shared resource in their pending offers.
  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillOnce(DoDefault());

  driver1.killTask(task.task_id());

  // Advance the clock until the allocator allocates
  // the recovered resources.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Resume the clock so the terminating task and executor can be reaped.
  Clock::resume();

  AWAIT_READY(offers1);

  offer1 = offers1.get()[0];

  EXPECT_TRUE(Resources(offer1.resources()).contains(
      allocatedResources(volume1, frameworkInfo1.role())));
  EXPECT_TRUE(Resources(offer1.resources()).contains(
      allocatedResources(volume2, frameworkInfo1.role())));

  // 5. DESTROY both the shared volumes via framework2 which would result
  //    in framework1 being rescinded the offer.
  Future<Nothing> rescinded;
  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .WillOnce(FutureSatisfy(&rescinded));

  driver2.acceptOffers(
      {offer2.id()},
      {DESTROY(allVolumes)},
      filters);

  AWAIT_READY(rescinded);

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This test verifies that the master recovers after a failover and
// re-offers the shared persistent volume when tasks using the same
// volume are still running.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeMasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Create the framework with SHARED_RESOURCES capability.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  Try<Resources> taskResources = Resources::parse("cpus:0.5;mem:512");

  TaskInfo task1 = createTask(
      offer1.slave_id(),
      taskResources.get() + volume,
      "sleep 1000");

  TaskInfo task2 = createTask(
      offer1.slave_id(),
      taskResources.get() + volume,
      "sleep 1000");

  // We should receive a TASK_RUNNING for each of the tasks.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume),
       LAUNCH({task1, task2})});

  AWAIT_READY(checkpointResources);
  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  // This is to make sure CheckpointResourcesMessage is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate failed over master by restarting the master.
  master->reset();

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  master = StartMaster();
  ASSERT_SOME(master);

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregistered);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  // Verify the offer from the failed over master.
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(
          Resources::parse("cpus:1;mem:1024").get(),
          frameworkInfo.role())));
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that DESTROY of non-shared persistent volume succeeds
// when a shared persistent volume is in use. This is to catch any
// regression to MESOS-6444.
TEST_P(PersistentVolumeTest, DestroyPersistentVolumeMultipleTasks)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Expect an offer from the slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource sharedVolume = createPersistentVolume(
      getDiskResource(Megabytes(2048), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal(),
      true); // Shared.

  Resource nonSharedVolume = createPersistentVolume(
      getDiskResource(Megabytes(2048), 2),
      "id2",
      "path2",
      None(),
      frameworkInfo.principal());

  // Create a long-lived task using a shared volume.
  Resources taskResources1 = Resources::parse(
      "cpus:1;mem:128").get() + sharedVolume;

  TaskInfo task1 = createTask(
      offer.slave_id(),
      taskResources1,
      "sleep 1000");

  // Create a short-lived task using a non-shared volume.
  Resources taskResources2 = Resources::parse(
      "cpus:1;mem:256").get() + nonSharedVolume;

  TaskInfo task2 = createTask(
      offer.slave_id(),
      taskResources2,
      "exit 0");

  const hashset<TaskID> tasks{task1.task_id(), task2.task_id()};

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We should receive a TASK_RUNNING each of the 2 tasks. We track task
  // termination by a TASK_FINISHED for the short-lived task.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(sharedVolume),
       CREATE(nonSharedVolume),
       LAUNCH({task1, task2})},
      filters);

  // Wait for TASK_RUNNING for both the tasks, and TASK_FINISHED for
  // the short-lived task.
  AWAIT_READY(status1);
  AWAIT_READY(status2);
  AWAIT_READY(status3);

  hashset<TaskID> tasksRunning;
  hashset<TaskID> tasksFinished;
  vector<Future<TaskStatus>> statuses{status1, status2, status3};

  foreach (const Future<TaskStatus>& status, statuses) {
    if (status->state() == TASK_RUNNING) {
      tasksRunning.insert(status->task_id());
    } else {
      tasksFinished.insert(status->task_id());
    }
  }

  ASSERT_EQ(tasks, tasksRunning);
  EXPECT_EQ(1u, tasksFinished.size());
  EXPECT_EQ(task2.task_id(), *(tasksFinished.begin()));

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // TODO(bmahler): This lambda is copied in several places
  // in the code, consider how best to pull this out.
  auto unallocated = [](const Resources& resources) {
    Resources result = resources;
    result.unallocate();
    return result;
  };

  // Check that the persistent volumes are offered back. The shared volume
  // is offered since it can be used in multiple tasks; the non-shared
  // volume is offered since there are no tasks using it.
  EXPECT_TRUE(unallocated(offer.resources()).contains(sharedVolume));
  EXPECT_TRUE(unallocated(offer.resources()).contains(nonSharedVolume));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Destroy the non-shared persistent volume since no task is using it.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(nonSharedVolume)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the shared persistent volume is in the offer, but the
  // non-shared volume is not in the offer.
  EXPECT_TRUE(unallocated(offer.resources()).contains(sharedVolume));
  EXPECT_FALSE(unallocated(offer.resources()).contains(nonSharedVolume));

  // We kill the long-lived task and wait for TASK_KILLED, so we can
  // DESTROY the persistent volume once the task terminates.
  Future<TaskStatus> status4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status4));

  driver.killTask(task1.task_id());

  AWAIT_READY(status4);
  EXPECT_EQ(task1.task_id(), status4->task_id());
  EXPECT_EQ(TASK_KILLED, status4->state());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Destroy the shared persistent volume.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(sharedVolume)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volumes are not in the offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(sharedVolume));
  EXPECT_FALSE(Resources(offer.resources()).contains(nonSharedVolume));

  driver.stop();
  driver.join();
}


// This test verifies that multiple iterations of CREATE and LAUNCH
// for the same framework is successfully handled in different
// ACCEPT calls.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeMultipleIterations)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // 1. Create framework so that all resources are offered to this framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // 2. The framework CREATEs the 1st shared volume, and LAUNCHes a task
  //    which uses this shared volume.
  Resource volume1 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume1,
      "sleep 1000");

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // We use a filter of 0 seconds so the resources will be available
  // in the next allocation cycle.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume1),
       LAUNCH({task1})},
      filters);

  // Advance the clock to generate an offer from the recovered resources.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);

  offer = offers.get()[0];

  // TODO(bmahler): This lambda is copied in several places
  // in the code, consider how best to pull this out.
  auto unallocated = [](const Resources& resources) {
    Resources result = resources;
    result.unallocate();
    return result;
  };

  EXPECT_TRUE(unallocated(offer.resources()).contains(volume1));

  // 3. The framework CREATEs the 2nd shared volume, and LAUNCHes a task
  //    using this shared volume.
  Resource volume2 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 2),
      "id2",
      "path2",
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume2,
      "sleep 1000");

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume2),
       LAUNCH({task2})},
      filters);

  // Advance the clock to generate an offer from the recovered resources.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);

  offer = offers.get()[0];

  EXPECT_TRUE(unallocated(offer.resources()).contains(volume1));
  EXPECT_TRUE(unallocated(offer.resources()).contains(volume2));

  driver.stop();
  driver.join();

  // Resume the clock so the terminating task and executor can be reaped.
  Clock::resume();
}


// This test verifies that persistent volumes are recovered properly
// after the slave restarts. The idea is to launch a command which
// keeps testing if the persistent volume exists, and fails if it does
// not. So the framework should not receive a TASK_FAILED after the
// slave finishes recovery.
TEST_P(PersistentVolumeTest, SlaveRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  // Create a task which tests for the existence of
  // the persistent volume directory.
  Resources taskResources = Resources::parse("cpus:1;mem:128").get() + volume;

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
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Restart the slave.
  slave.get()->terminate();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(reregisterExecutorMessage);

  // Wait for slave to schedule reregister timeout.
  Clock::settle();

  // Ensure the slave considers itself recovered.
  Clock::advance(slaveFlags.executor_reregistration_timeout);

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
  EXPECT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_KILLED, status2->state());

  driver.stop();
  driver.join();
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
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  create->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that the principal of `DEFAULT_CREDENTIAL`
  // can destroy its own persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  destroy->mutable_creator_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<CheckpointResourcesMessage> checkpointResources1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the CheckpointResourceMessage response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(checkpointResources1);
  EXPECT_EQ(Resources(checkpointResources1->resources()), volume);

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was created successfully.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<CheckpointResourcesMessage> checkpointResources2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  // Destroy the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  AWAIT_READY(checkpointResources2);
  EXPECT_FALSE(
      Resources(checkpointResources2->resources()).contains(volume));

  // Expect an offer that does not contain the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is not in the offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));

  driver.stop();
  driver.join();
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
  // principal) can create persistent volumes for any role.
  mesos::ACL::CreateVolume* create = acls.add_create_volumes();
  create->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that any principal (and also frameworks without a
  // principal) can destroy persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  destroy->mutable_creator_principals()->set_type(mesos::ACL::Entity::ANY);

  // We use the filter explicitly here so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` with no principal.
  FrameworkInfo frameworkInfo;
  frameworkInfo.set_name("no-principal");
  frameworkInfo.set_user(os::user().get());
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  // Create a master. Since the framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.role();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      None());

  Future<CheckpointResourcesMessage> checkpointResources1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the CheckpointResourceMessage response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(checkpointResources1);
  EXPECT_EQ(Resources(checkpointResources1->resources()), volume);

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was successfully created.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<CheckpointResourcesMessage> checkpointResources2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

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
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was not created
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));
  EXPECT_FALSE(
      Resources(checkpointResources2->resources()).contains(volume));

  driver.stop();
  driver.join();
}

// TODO(greggomann): Change the names of `driver1` and `driver2` below.

// This test verifies that `create` and `destroy` operations fail as expected
// when authorization fails and no principal is supplied.
TEST_P(PersistentVolumeTest, BadACLNoPrincipal)
{
  // Manipulate the clock manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This ACL declares that the principal of `DEFAULT_FRAMEWORK_INFO`
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* create1 = acls.add_create_volumes();
  create1->mutable_principals()->add_values(DEFAULT_FRAMEWORK_INFO.principal());
  create1->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that any other principals
  // cannot create persistent volumes.
  mesos::ACL::CreateVolume* create2 = acls.add_create_volumes();
  create2->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create2->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // This ACL declares that no principal can destroy persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  destroy->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);

  // We use this filter so that resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` with no principal.
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("no-principal");
  frameworkInfo1.set_user(os::user().get());
  frameworkInfo1.set_role(DEFAULT_TEST_ROLE);

  // Create a `FrameworkInfo` with a principal.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_role(DEFAULT_TEST_ROLE);

  // Create a master. Since one framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.role();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get()->pid);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  {
    Resource volume = createPersistentVolume(
        getDiskResource(Megabytes(2048)),
        "id1",
        "path1",
        None(),
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
    EXPECT_FALSE(offers->empty());

    offer = offers.get()[0];

    // Check that the persistent volume is not contained in this offer.
    EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.role())));
  }

  // Decline the offer and suppress so the second
  // framework will receive the offer instead.
  driver1.declineOffer(offer.id(), filters);
  driver1.suppressOffers();

  // Create a second framework which can create volumes.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get()->pid);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Expect an offer to the second framework.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo2.principal());

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
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo2.role())));

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
  EXPECT_FALSE(offers->empty());

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
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is still contained in this offer.
  // TODO(greggomann): In addition to checking that the volume is contained in
  // the offer, we should also confirm that the Destroy operation failed for the
  // correct reason. See MESOS-5470.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.role())));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
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
  // can create persistent volumes for any role.
  mesos::ACL::CreateVolume* create1 = acls.add_create_volumes();
  create1->mutable_principals()->add_values("creator-principal");
  create1->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This ACL declares that all other principals
  // cannot create any persistent volumes.
  mesos::ACL::CreateVolume* create2 = acls.add_create_volumes();
  create2->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  create2->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // This ACL declares that no principal can destroy persistent volumes.
  mesos::ACL::DestroyVolume* destroy = acls.add_destroy_volumes();
  destroy->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  destroy->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a `FrameworkInfo` that cannot create or destroy volumes.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_role(DEFAULT_TEST_ROLE);

  // Create a `FrameworkInfo` that can create volumes.
  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("creator-framework");
  frameworkInfo2.set_user(os::user().get());
  frameworkInfo2.set_role(DEFAULT_TEST_ROLE);
  frameworkInfo2.set_principal("creator-principal");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.role();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler/framework.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get()->pid);

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
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  {
    Resource volume = createPersistentVolume(
        getDiskResource(Megabytes(2048)),
        "id1",
        "path1",
        None(),
        frameworkInfo1.principal());

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
    EXPECT_FALSE(offers->empty());

    offer = offers.get()[0];

    // Check that the persistent volume is not contained in this offer.
    EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.role())));
  }

  // Decline the offer and suppress so the second
  // framework will receive the offer instead.
  driver1.declineOffer(offer.id(), filters);
  driver1.suppressOffers();

  // Create a second framework which can create volumes.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get()->pid);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Expect an offer to the second framework.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo2.principal());

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
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo2.role())));

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
  EXPECT_FALSE(offers->empty());

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
  EXPECT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is still contained in this offer.
  // TODO(greggomann): In addition to checking that the volume is contained in
  // the offer, we should also confirm that the Destroy operation failed for the
  // correct reason. See MESOS-5470.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.role())));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
