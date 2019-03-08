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
#include <set>
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

#include "common/resources_utils.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registry_operations.hpp"

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
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::WithParamInterface;

namespace process {

void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readonlyAuthenticationRealm,
    const Option<string>& readwriteAuthenticationRealm);

} // namespace process {


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
  void SetUp() override
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

  void TearDown() override
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
    set<string> roles;

    foreach (const FrameworkInfo& framework, frameworks) {
      mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
      acl->mutable_principals()->add_values(framework.principal());

      foreach (const string& role, protobuf::framework::getRoles(framework)) {
        acl->mutable_roles()->add_values(role);
        roles.insert(role);
      }
    }

    flags.acls = acls;
    flags.roles = strings::join(",", roles);

    return flags;
  }

  // Depending on the agent capability, the master will send different
  // messages to the agent when a persistent volume is applied.
  template<typename To>
  Future<Resources> getOperationMessage(To to)
  {
    return FUTURE_PROTOBUF(ApplyOperationMessage(), _, to)
      .then([](const ApplyOperationMessage& message) {
        switch (message.operation_info().type()) {
          case Offer::Operation::UNKNOWN:
          case Offer::Operation::LAUNCH:
          case Offer::Operation::LAUNCH_GROUP:
          case Offer::Operation::RESERVE:
          case Offer::Operation::UNRESERVE:
          case Offer::Operation::CREATE_DISK:
          case Offer::Operation::DESTROY_DISK:
          case Offer::Operation::GROW_VOLUME:
          case Offer::Operation::SHRINK_VOLUME:
            UNREACHABLE();
          case Offer::Operation::CREATE: {
            Resources resources = message.operation_info().create().volumes();
            resources.unallocate();

            return resources;
          }
          case Offer::Operation::DESTROY: {
            Resources resources = message.operation_info().destroy().volumes();
            resources.unallocate();

            return resources;
          }
        }

        UNREACHABLE();
      });
  }

  // Creates a disk with / without a `source` based on the
  // parameterization of the test. `id` influences the `root` if one
  // is specified so that we can create multiple disks in the tests.
  Resource getDiskResource(const Bytes& mb, size_t id = 1)
  {
    CHECK_LE(1u, id);
    CHECK_GE(NUM_DISKS, id);
    Resource diskResource;

    switch (GetParam()) {
      case NONE: {
        diskResource = createDiskResource(
            stringify((double) mb.bytes() / Bytes::MEGABYTES),
            DEFAULT_TEST_ROLE,
            None(),
            None());

        break;
      }
      case PATH: {
        diskResource = createDiskResource(
            stringify((double) mb.bytes() / Bytes::MEGABYTES),
            DEFAULT_TEST_ROLE,
            None(),
            None(),
            createDiskSourcePath(path::join(diskPath, "disk" + stringify(id))));

        break;
      }
      case MOUNT: {
        diskResource = createDiskResource(
            stringify((double) mb.bytes() / Bytes::MEGABYTES),
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
        PersistentVolumeSourceType::NONE, PersistentVolumeSourceType::PATH));


// We also want to parameterize them for `MOUNT`. On linux this means
// using `tmpfs`.
#ifdef __linux__
// On linux we have to run this test as root, as we need permissions
// to access `tmpfs`.
INSTANTIATE_TEST_CASE_P(
    ROOT_MountDiskResource,
    PersistentVolumeTest,
    ::testing::Values(PersistentVolumeSourceType::MOUNT));
#else // __linux__
// Otherwise we can run it without root privileges as we just require
// a directory.
INSTANTIATE_TEST_CASE_P(
    MountDiskResource,
    PersistentVolumeTest,
    ::testing::Values(PersistentVolumeSourceType::MOUNT));
#endif // __linux__


// This test verifies that the slave checkpoints resources
// when the framework creates/destroys persistent volumes.
TEST_P(PersistentVolumeTest, CreateAndDestroyPersistentVolumes)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Future<Resources> message3 = getOperationMessage(slave.get()->pid);
  Future<Resources> message2 = getOperationMessage(slave.get()->pid);
  Future<Resources> message1 = getOperationMessage(slave.get()->pid);

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
  EXPECT_TRUE(message1->contains(volume1));

  AWAIT_READY(message2);
  EXPECT_TRUE(message2->contains(volume2));

  // Ensure that the messages reach the slave.
  Clock::settle();

  EXPECT_TRUE(os::exists(volume1Path));
  EXPECT_TRUE(os::exists(volume2Path));

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Expect that the offer contains the persistent volumes we created.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume1, frameworkInfo.roles(0))));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume2, frameworkInfo.roles(0))));

  // Destroy `volume1`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume1)},
      filters);

  AWAIT_READY(message3);
  EXPECT_TRUE(message3->contains(volume1));

  // Ensure that the messages reach the slave.
  Clock::settle();

  // For MOUNT disks, we preserve the top-level volume directory (but
  // delete all of the files and subdirectories underneath it). For
  // non-MOUNT disks, the volume directory should be removed when the
  // volume is destroyed.
  if (GetParam() == MOUNT) {
    EXPECT_TRUE(os::exists(volume1Path));

    Try<list<string>> files = ::fs::list(path::join(volume1Path, "*"));
    ASSERT_SOME(files);
    EXPECT_TRUE(files->empty());
  } else {
    EXPECT_FALSE(os::exists(volume1Path));
  }

  EXPECT_TRUE(os::exists(volume2Path));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that a framework can grow a persistent volume and use the
// grown volume afterward.
TEST_P(PersistentVolumeTest, GrowVolume)
{
  if (GetParam() == MOUNT) {
    // It is not possible to have a valid `GrowVolume` on a MOUNT disk because
    // the volume must use up all disk space at `Create` and no space will be
    // left for `addition`. Therefore we skip this test.
    // TODO(zhitao): Make MOUNT a meaningful parameter value for this test, or
    // create a new fixture to avoid testing against it.
    return;
  }

  Clock::pause();

  // Register a framework with role "default-role/foo" for dynamic reservations.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, strings::join("/", DEFAULT_TEST_ROLE, "foo"));

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();

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

  Future<vector<Offer>> offersBeforeCreate;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeCreate));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeCreate);
  ASSERT_FALSE(offersBeforeCreate->empty());

  Offer offer = offersBeforeCreate->at(0);

  // The disk spaces will be merged if the fixture parameter is `NONE`.
  Bytes totalBytes = GetParam() == NONE ? Megabytes(4096) : Megabytes(2048);

  Bytes additionBytes = Megabytes(512);

  // Construct a dynamic reservation for all disk resources.
  // NOTE: We dynamically reserve all disk resources so they become checkpointed
  // resources and thus will be verified on the agent when launching a task.
  Resource::ReservationInfo dynamicReservation = createDynamicReservationInfo(
      frameworkInfo.roles(0),
      frameworkInfo.principal());

  Resource dynamicallyReserved = getDiskResource(totalBytes, 1);
  dynamicallyReserved.add_reservations()->CopyFrom(dynamicReservation);

  // Construct a persistent volume which does not use up all disk resources.
  Resource volume = createPersistentVolume(
      getDiskResource(totalBytes - additionBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());
  volume.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(volume));

  Resource addition = getDiskResource(additionBytes, 1);
  addition.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(addition));

  Resource grownVolume = createPersistentVolume(
      getDiskResource(totalBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());
  grownVolume.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(grownVolume));

  Future<vector<Offer>> offersBeforeGrow;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeGrow));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create the persistent volume.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReserved), CREATE(volume)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeGrow);
  ASSERT_FALSE(offersBeforeGrow->empty());

  offer = offersBeforeGrow->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
      allocatedResources(addition, frameworkInfo.roles(0))));

  // Make sure the volume exists, and leave a non-empty file there.
  string volumePath = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir, volume);

  EXPECT_TRUE(os::exists(volumePath));
  string filePath = path::join(volumePath, "file");
  ASSERT_SOME(os::write(filePath, "abc"));

  Future<vector<Offer>> offersAfterGrow;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterGrow));

  // Grow the volume.
  driver.acceptOffers(
      {offer.id()},
      {GROW_VOLUME(volume, addition)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterGrow);
  ASSERT_FALSE(offersAfterGrow->empty());

  offer = offersAfterGrow->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(grownVolume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_FALSE(
      Resources(offer.resources()).contains(
      allocatedResources(addition, frameworkInfo.roles(0))));

  Future<TaskStatus> taskStarting;
  Future<TaskStatus> taskRunning;
  Future<TaskStatus> taskFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskStarting))
    .WillOnce(FutureArg<1>(&taskRunning))
    .WillOnce(FutureArg<1>(&taskFinished));

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test `cat path1/file` = abc");

  // Launch a task to verify that `GROW_VOLUME` takes effect on the agent and
  // the task can use the grown volume.
  driver.acceptOffers({offer.id()}, {LAUNCH({task})}, filters);

  AWAIT_READY(taskStarting);
  EXPECT_EQ(task.task_id(), taskStarting->task_id());
  EXPECT_EQ(TASK_STARTING, taskStarting->state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(task.task_id(), taskRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, taskRunning->state());

  AWAIT_READY(taskFinished);
  EXPECT_EQ(task.task_id(), taskFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, taskFinished->state());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that a framework can shrink a persistent volume and use
// the shrunk volume afterward.
TEST_P(PersistentVolumeTest, ShrinkVolume)
{
  Clock::pause();

  // Register a framework with role "default-role/foo" for dynamic reservations.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, strings::join("/", DEFAULT_TEST_ROLE, "foo"));

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();

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

  Future<vector<Offer>> offersBeforeCreate;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeCreate));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeCreate);
  ASSERT_FALSE(offersBeforeCreate->empty());

  Offer offer = offersBeforeCreate->at(0);

  // The disk spaces will be merged if the fixture parameter is `NONE`.
  Bytes totalBytes = GetParam() == NONE ? Megabytes(4096) : Megabytes(2048);

  Bytes subtractBytes = Megabytes(512);

  // Construct a dynamic reservation for all disk resources.
  // NOTE: We dynamically reserve all disk resources so they become checkpointed
  // resources and thus will be verified on the agent when launching a task.
  Resource::ReservationInfo dynamicReservation = createDynamicReservationInfo(
      frameworkInfo.roles(0),
      frameworkInfo.principal());

  Resource dynamicallyReserved = getDiskResource(totalBytes, 1);
  dynamicallyReserved.add_reservations()->CopyFrom(dynamicReservation);

  // Construct a persistent volume which uses up all disk resources.
  Resource volume = createPersistentVolume(
      getDiskResource(totalBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());
  volume.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(volume));

  Resource subtract = getDiskResource(subtractBytes, 1);
  subtract.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(subtract));

  Resource shrunkVolume = createPersistentVolume(
      getDiskResource(totalBytes - subtractBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());
  shrunkVolume.add_reservations()->CopyFrom(dynamicReservation);
  ASSERT_TRUE(needCheckpointing(shrunkVolume));

  Future<vector<Offer>> offersBeforeShrink;

  // Expect an offer containing the original volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeShrink));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create the persistent volume.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReserved), CREATE(volume)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeShrink);
  ASSERT_FALSE(offersBeforeShrink->empty());

  offer = offersBeforeShrink->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_FALSE(
      Resources(offer.resources()).contains(
      allocatedResources(subtract, frameworkInfo.roles(0))));

  // Make sure the volume exists, and leaves a non-empty file there.
  string volumePath = slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir, volume);

  EXPECT_TRUE(os::exists(volumePath));
  string filePath = path::join(volumePath, "file");
  ASSERT_SOME(os::write(filePath, "abc"));

  Future<vector<Offer>> offersAfterShrink;

  // Expect an offer containing shrunk volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterShrink));

  driver.acceptOffers(
      {offer.id()},
      {SHRINK_VOLUME(volume, subtract.scalar())},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterShrink);
  ASSERT_FALSE(offersAfterShrink->empty());

  offer = offersAfterShrink->at(0);

  if (GetParam() != MOUNT) {
    EXPECT_EQ(
        allocatedResources(Resources(shrunkVolume), frameworkInfo.roles(0)),
        Resources(offer.resources()).persistentVolumes());

    EXPECT_TRUE(
        Resources(offer.resources()).contains(
        allocatedResources(subtract, frameworkInfo.roles(0))));
  } else {
    EXPECT_EQ(
        allocatedResources(Resources(volume), frameworkInfo.roles(0)),
        Resources(offer.resources()).persistentVolumes());

    EXPECT_FALSE(
        Resources(offer.resources()).contains(
        allocatedResources(subtract, frameworkInfo.roles(0))));
  }

  Future<TaskStatus> taskStarting;
  Future<TaskStatus> taskRunning;
  Future<TaskStatus> taskFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskStarting))
    .WillOnce(FutureArg<1>(&taskRunning))
    .WillOnce(FutureArg<1>(&taskFinished));

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test `cat path1/file` = abc");

  // Launch a task to verify that: if the fixture parameter is NONE or PATH,
  // `SHRINK_VOLUME` takes effect on the agent and the task can use the shrunk
  // volume as well as the freed disk resource; otherwise, `SHRINK_VOLUME`
  // takes no effect on the agent.
  driver.acceptOffers({offer.id()}, {LAUNCH({task})}, filters);

  AWAIT_READY(taskStarting);
  EXPECT_EQ(task.task_id(), taskStarting->task_id());
  EXPECT_EQ(TASK_STARTING, taskStarting->state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(task.task_id(), taskRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, taskRunning->state());

  AWAIT_READY(taskFinished);
  EXPECT_EQ(task.task_id(), taskFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, taskFinished->state());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that any task to launch after a `GROW_VOLUME` in the same
// `ACCEPT` call is dropped if the task consumes the original or grown volume,
// because we intend to make `GROW_VOLUME` non-speculative.
TEST_P(PersistentVolumeTest, NonSpeculativeGrowAndLaunch)
{
  if (GetParam() == MOUNT) {
    // It is not possible to have a valid `GrowVolume` on a MOUNT disk because
    // the volume must use up all disk space at `Create` and no space will be
    // left for `addition`. Therefore we skip this test.
    // TODO(zhitao): Make MOUNT a meaningful parameter value for this test, or
    // create a new fixture to avoid testing against it.
    return;
  }

  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();

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

  Future<vector<Offer>> offersBeforeOperations;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeOperations));

  driver.start();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeOperations);
  ASSERT_FALSE(offersBeforeOperations->empty());

  Offer offer = offersBeforeOperations->at(0);

  // Disk spaces will be merged if fixture parameter is `NONE`.
  Bytes totalBytes = GetParam() == NONE ? Megabytes(4096) : Megabytes(2048);

  Bytes additionBytes = Megabytes(512);

  // Construct a persistent volume which do not use up all disk resources.
  Resource volume = createPersistentVolume(
      getDiskResource(totalBytes - additionBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Resource addition = getDiskResource(additionBytes, 1);

  Resource grownVolume = createPersistentVolume(
      getDiskResource(totalBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<TaskStatus> taskError1;
  Future<TaskStatus> taskError2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskError1))
    .WillOnce(FutureArg<1>(&taskError2));

  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + grownVolume,
      "echo abc > path1/file");

  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume,
      "echo abc > path1/file");

  Future<vector<Offer>> offersAfterOperations;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterOperations));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // The create and grow volume operations will succeed, but the tasks will be
  // dropped with `TASK_ERROR`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), GROW_VOLUME(volume, addition), LAUNCH({task1, task2})},
      filters);

  AWAIT_READY(taskError1);
  AWAIT_READY(taskError2);

  hashset<TaskID> expectedTasks = {task1.task_id(), task2.task_id()};
  hashset<TaskID> actualTasks = {taskError1->task_id(), taskError2->task_id()};
  EXPECT_EQ(expectedTasks, actualTasks);
  EXPECT_EQ(TASK_ERROR, taskError1->state());
  EXPECT_EQ(TASK_ERROR, taskError2->state());

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterOperations);
  ASSERT_FALSE(offersAfterOperations->empty());

  offer = offersAfterOperations->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(grownVolume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that any task to launch after a `SHRINK_VOLUME` in the
// same `ACCEPT` call is dropped if the task consumes the original or shrunk
// volume, because we intend to make `SHRINK_VOLUME` non-speculative.
TEST_P(PersistentVolumeTest, NonSpeculativeShrinkAndLaunch)
{
  if (GetParam() == MOUNT) {
    // It is not possible to have a valid `GrowVolume` on a MOUNT disk because
    // the volume must use up all disk space at `Create` and no space will be
    // left for `addition`. Therefore we skip this test.
    // TODO(zhitao): Make MOUNT a meaningful parameter value for this test, or
    // create a new fixture to avoid testing against it.
    return;
  }

  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
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

  Future<vector<Offer>> offersBeforeOperations;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeOperations));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeOperations);
  ASSERT_FALSE(offersBeforeOperations->empty());

  Offer offer = offersBeforeOperations->at(0);

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(1024), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Value::Scalar subtract;
  subtract.set_value(512);

  Resource shrunkVolume = createPersistentVolume(
      getDiskResource(Megabytes(512), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<TaskStatus> taskError1;
  Future<TaskStatus> taskError2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskError1))
    .WillOnce(FutureArg<1>(&taskError2));

  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + shrunkVolume,
      "echo abc > path1/file");

  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume,
      "echo abc > path1/file");

  Future<vector<Offer>> offersAfterOperations;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterOperations));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // The create and shrink volume operations will succeed, but the tasks will be
  // dropped with `TASK_ERROR`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), SHRINK_VOLUME(volume, subtract), LAUNCH({task1, task2})},
      filters);

  AWAIT_READY(taskError1);
  AWAIT_READY(taskError2);

  hashset<TaskID> expectedTasks = {task1.task_id(), task2.task_id()};
  hashset<TaskID> actualTasks = {taskError1->task_id(), taskError2->task_id()};
  EXPECT_EQ(expectedTasks, actualTasks);
  EXPECT_EQ(TASK_ERROR, taskError1->state());
  EXPECT_EQ(TASK_ERROR, taskError2->state());

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterOperations);
  ASSERT_FALSE(offersAfterOperations->empty());
  offer = offersAfterOperations->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(shrunkVolume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that grow and shrink operations can complete
// successfully when authorization succeeds.
TEST_P(PersistentVolumeTest, GoodACLGrowThenShrink)
{
  if (GetParam() == MOUNT) {
    // It is not possible to have a valid `GrowVolume` on a MOUNT disk because
    // the volume must use up all disk space at `Create` and no space will be
    // left for `addition`. Therefore we skip this test.
    // TODO(zhitao): Make MOUNT a meaningful parameter value for this test, or
    // create a new fixture to avoid testing against it.
    return;
  }

  Clock::pause();

  ACLs acls;

  // This ACL declares that the principal of `DEFAULT_CREDENTIAL`
  // can resize persistent volumes for DEFAULT_TEST_ROLE.
  mesos::ACL::ResizeVolume* resize = acls.add_resize_volumes();
  resize->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  resize->mutable_roles()->add_values(DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offersBeforeCreate;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersBeforeCreate));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeCreate);
  ASSERT_FALSE(offersBeforeCreate->empty());

  Offer offer = offersBeforeCreate->at(0);

  // The disk spaces will be merged if the fixture parameter is `NONE`.
  Bytes totalBytes = GetParam() == NONE ? Megabytes(4096) : Megabytes(2048);

  Bytes bytesDifference = Megabytes(512);

  // Construct a persistent volume which do not use up all disk resources.
  Resource volume = createPersistentVolume(
      getDiskResource(totalBytes - bytesDifference, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Resource difference = getDiskResource(bytesDifference, 1);

  Resource grownVolume = createPersistentVolume(
      getDiskResource(totalBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<vector<Offer>> offersAfterGrow;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterGrow));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Create a persistent volume then grow it.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), GROW_VOLUME(volume, difference)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterGrow);
  ASSERT_FALSE(offersAfterGrow->empty());

  offer = offersAfterGrow->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(grownVolume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_FALSE(
      Resources(offer.resources()).contains(
      allocatedResources(difference, frameworkInfo.roles(0))));

  Future<vector<Offer>> offersAfterShrink;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersAfterShrink));

  // Shrink the volume back to original size.
  driver.acceptOffers(
      {offer.id()},
      {SHRINK_VOLUME(grownVolume, difference.scalar())},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterShrink);
  ASSERT_FALSE(offersAfterShrink->empty());
  offer = offersAfterShrink->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), frameworkInfo.roles(0)),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
      allocatedResources(difference, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();

  Clock::resume();
}

// This test verifies that grow and shrink operations get dropped if
// authorization fails and no principal is supplied.
TEST_P(PersistentVolumeTest, BadACLDropGrowAndShrink)
{
  if (GetParam() == MOUNT) {
    // It is not possible to have a valid `GrowVolume` on a MOUNT disk because
    // the volume must use up all disk space at `Create` and no space will be
    // left for `addition`. Therefore we skip this test.
    // TODO(zhitao): Make MOUNT a meaningful parameter value for this test, or
    // create a new fixture to avoid testing against it.
    return;
  }

  Clock::pause();

  ACLs acls;

  // This ACL declares that no principal can resize any volume.
  mesos::ACL::ResizeVolume* resize = acls.add_resize_volumes();
  resize->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  resize->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // DEFAULT_FRAMEWORK_INFO uses DEFAULT_CREDENTIAL.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  Future<vector<Offer>> offersBeforeCreate;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offersBeforeCreate));

  driver1.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeCreate);
  ASSERT_FALSE(offersBeforeCreate->empty());

  Offer offer = offersBeforeCreate->at(0);

  // Disk spaces will be merged if fixture parameter is `NONE`.
  Bytes totalBytes = GetParam() == NONE ? Megabytes(4096) : Megabytes(2048);

  Bytes bytesDifference = Megabytes(512);

  // Construct a persistent volume which does not use up all disk resources.
  Resource volume = createPersistentVolume(
      getDiskResource(totalBytes - bytesDifference, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo1.principal());

  Resource difference = getDiskResource(bytesDifference, 1);

  Resource grownVolume = createPersistentVolume(
      getDiskResource(totalBytes, 1),
      "id1",
      "path1",
      None(),
      frameworkInfo1.principal());

  Future<vector<Offer>> offersAfterGrow1;

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offersAfterGrow1));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Creating the persistent volume will succeed, but growing will fail due to
  // ACL.
  driver1.acceptOffers(
      {offer.id()},
      {CREATE(volume), GROW_VOLUME(volume, difference)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterGrow1);
  ASSERT_FALSE(offersAfterGrow1->empty());

  offer = offersAfterGrow1->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), DEFAULT_TEST_ROLE),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
      allocatedResources(difference, DEFAULT_TEST_ROLE)));

  Future<vector<Offer>> offersAfterShrink1;

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offersAfterShrink1));

  driver1.acceptOffers(
      {offer.id()},
      {SHRINK_VOLUME(volume, difference.scalar())},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterShrink1);
  ASSERT_FALSE(offersAfterShrink1->empty());

  offer = offersAfterShrink1->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), DEFAULT_TEST_ROLE),
      Resources(offer.resources()).persistentVolumes());

  driver1.stop();
  driver1.join();

  // Start the second framework with no principal.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.clear_principal();
  frameworkInfo2.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  Future<vector<Offer>> offersBeforeGrow2;
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offersBeforeGrow2));

  driver2.start();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersBeforeGrow2);
  ASSERT_FALSE(offersBeforeGrow2->empty());

  offer = offersBeforeGrow2->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), DEFAULT_TEST_ROLE),
      Resources(offer.resources()).persistentVolumes());

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
      allocatedResources(difference, DEFAULT_TEST_ROLE)));

  Future<vector<Offer>> offersAfterGrow2;

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offersAfterGrow2));

  driver2.acceptOffers(
      {offer.id()},
      {GROW_VOLUME(volume, difference)},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterGrow2);
  ASSERT_FALSE(offersAfterGrow2->empty());
  offer = offersAfterGrow2->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), DEFAULT_TEST_ROLE),
      Resources(offer.resources()).persistentVolumes());

  Future<vector<Offer>> offersAfterShrink2;

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offersAfterShrink2));

  driver2.acceptOffers(
      {offer.id()},
      {SHRINK_VOLUME(volume, difference.scalar())},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterShrink2);
  ASSERT_FALSE(offersAfterShrink2->empty());
  offer = offersAfterShrink2->at(0);

  EXPECT_EQ(
      allocatedResources(Resources(volume), DEFAULT_TEST_ROLE),
      Resources(offer.resources()).persistentVolumes());

  driver2.stop();
  driver2.join();

  Clock::resume();
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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(message);

  // Restart the slave.
  slave.get()->terminate();

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlave);

  ReregisterSlaveMessage reregisterSlave_ = reregisterSlave.get();
  upgradeResources(&reregisterSlave_);

  EXPECT_EQ(Resources(reregisterSlave_.checkpointed_resources()), volume);

  driver.stop();
  driver.join();
}


TEST_P(PersistentVolumeTest, PreparePersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(message);

  // This is to make sure the operation message is processed.
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

  Future<UpdateSlaveMessage> updateSlaveMessage1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage1);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume)});

  AWAIT_READY(message);

  // This is to make sure the operation message is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate failed over master by restarting the master.
  master->reset();

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<UpdateSlaveMessage> updateSlaveMessage2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

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

  AWAIT_READY(updateSlaveMessage2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave1 = StartSlave(
      &detector,
      &containerizer,
      slaveFlags,
      true);

  ASSERT_SOME(slave1);
  ASSERT_NE(nullptr, slave1.get()->mock());

  slave1.get()->start();

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<Resources> message = getOperationMessage(_);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)});

  AWAIT_READY(message);

  slave1.get()->terminate();

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it's not compatible with the
  // checkpointed resources.
  slaveFlags.resources = "disk:1024";

  Try<Owned<cluster::Slave>> slave2 = StartSlave(
      &detector,
      &containerizer,
      slaveFlags,
      true);

  ASSERT_SOME(slave2);
  ASSERT_NE(nullptr, slave2.get()->mock());

  Future<Future<Nothing>> recover;
  EXPECT_CALL(*slave2.get()->mock(), __recover(_))
    .WillOnce(DoAll(FutureArg<0>(&recover), Return()));

  slave2.get()->start();

  AWAIT_READY(recover);
  AWAIT_FAILED(recover.get());

  slave2.get()->terminate();

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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers->empty());

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

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
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

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

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
      allocatedResources(volume, frameworkInfo.roles(0))));

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  driver.acceptOffers({offer.id()}, {DESTROY(volume)});

  AWAIT_READY(message);
  EXPECT_TRUE(message->contains(volume));

  // Ensure that operation message reaches the slave.
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
    EXPECT_TRUE(files->empty());
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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers->empty());

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

  // We should receive a TASK_STARTING, followed by a TASK_RUNNING
  // and a TASK_FINISHED for each of the 2 tasks.
  // We do not check for the actual task state since it's not the
  // primary objective of the test. We instead verify that the paths
  // are created by the tasks after we receive enough status updates.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;
  Future<TaskStatus> status5;
  Future<TaskStatus> status6;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4))
    .WillOnce(FutureArg<1>(&status5))
    .WillOnce(FutureArg<1>(&status6));

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
  AWAIT_READY(status5);
  AWAIT_READY(status6);

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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  // 1. Create framework1 so that all resources are offered to this framework.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers1->empty());

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

  // We don't care about the fate of the task in this test as we
  // express the expectations through offers.
  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

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
  frameworkInfo2.set_roles(0, DEFAULT_TEST_ROLE);
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
      allocatedResources(volume1, frameworkInfo2.roles(0))));
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume2, frameworkInfo2.roles(0))));

  // 4. framework1 kills the task which results in an offer to framework1
  //    with the shared volumes. At this point, both frameworks will have
  //    the shared resource in their pending offers.
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
      allocatedResources(volume1, frameworkInfo1.roles(0))));
  EXPECT_TRUE(Resources(offer1.resources()).contains(
      allocatedResources(volume2, frameworkInfo1.roles(0))));

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


// This test verifies that multiple frameworks belonging to the same role
// can use the same shared persistent volume to launch tasks simultaneously.
// It also verifies that metrics for used resources are correctly populated
// on the master and the agent.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeMultipleFrameworks)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  // 1. Create framework1 so that all resources are offered to this framework.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_roles(0, DEFAULT_TEST_ROLE);
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

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  // 2. framework1 CREATEs a shared volume, and LAUNCHes a task with a subset
  //    of resources from the offer.
  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo1.principal(),
      true); // Shared volume.

  // Create a task which uses a portion of the offered resources, so that
  // the remaining resources can be offered to framework2.
  TaskInfo task1 = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume,
      "echo abc > path1/file1 && sleep 1000");

  // We should receive a TASK_STARTING and a TASK_RUNNING for the launched task.
  Future<TaskStatus> status0;
  Future<TaskStatus> status1;

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1));

  // We use a filter of 0 seconds so the resources will be available
  // in the next allocation cycle.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver1.acceptOffers(
      {offer1.id()},
      {CREATE(volume),
       LAUNCH({task1})},
      filters);

  AWAIT_READY(status0);
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Collect metrics based on framework1.
  JSON::Object stats1 = Metrics();
  ASSERT_EQ(1u, stats1.values.count("master/cpus_used"));
  ASSERT_EQ(1u, stats1.values.count("master/mem_used"));
  ASSERT_EQ(1u, stats1.values.count("master/disk_used"));
  ASSERT_EQ(1u, stats1.values.count("master/disk_revocable_used"));
  EXPECT_EQ(1, stats1.values["master/cpus_used"]);
  EXPECT_EQ(128, stats1.values["master/mem_used"]);
  EXPECT_EQ(2048, stats1.values["master/disk_used"]);
  EXPECT_EQ(0, stats1.values["master/disk_revocable_used"]);
  ASSERT_EQ(1u, stats1.values.count("slave/cpus_used"));
  ASSERT_EQ(1u, stats1.values.count("slave/mem_used"));
  ASSERT_EQ(1u, stats1.values.count("slave/disk_used"));
  ASSERT_EQ(1u, stats1.values.count("slave/disk_revocable_used"));
  EXPECT_EQ(2048, stats1.values["slave/disk_used"]);
  EXPECT_EQ(0, stats1.values["slave/disk_revocable_used"]);

  // 3. Create framework2 of the same role. It would be offered resources
  //    recovered from the framework1 call.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume, frameworkInfo2.roles(0))));

  // 4. framework2 LAUNCHes a task with a subset of resources from the offer.

  // Create a task `task2` which uses the same shared volume as `task1`.
  TaskInfo task2 = createTask(
      offer2.slave_id(),
      Resources::parse("cpus:1;mem:256").get() + volume,
      "echo abc > path1/file2 && sleep 1000");

  // We should receive a TASK_STARTING and a TASK_RUNNING for the launched task.
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;

  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3));

  driver2.acceptOffers(
      {offer2.id()},
      {LAUNCH({task2})},
      filters);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_STARTING, status2->state());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3->state());

  // Collect metrics based on both frameworks. Note that the `cpus_used` and
  // `mem_used` is updated, but `disk_used` does not change since both tasks
  // use the same shared volume.
  JSON::Object stats2 = Metrics();
  ASSERT_EQ(1u, stats2.values.count("master/cpus_used"));
  ASSERT_EQ(1u, stats2.values.count("master/mem_used"));
  ASSERT_EQ(1u, stats2.values.count("master/disk_used"));
  ASSERT_EQ(1u, stats2.values.count("master/disk_revocable_used"));
  EXPECT_EQ(2, stats2.values["master/cpus_used"]);
  EXPECT_EQ(384, stats2.values["master/mem_used"]);
  EXPECT_EQ(2048, stats2.values["master/disk_used"]);
  EXPECT_EQ(0, stats2.values["master/disk_revocable_used"]);
  ASSERT_EQ(1u, stats2.values.count("slave/cpus_used"));
  ASSERT_EQ(1u, stats2.values.count("slave/mem_used"));
  ASSERT_EQ(1u, stats2.values.count("slave/disk_used"));
  ASSERT_EQ(1u, stats2.values.count("slave/disk_revocable_used"));
  EXPECT_EQ(2048, stats2.values["slave/disk_used"]);
  EXPECT_EQ(0, stats2.values["slave/disk_revocable_used"]);

  // Resume the clock so the terminating task and executor can be reaped.
  Clock::resume();

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This test verifies that a command task launched with a non-root user
// can write to a shared persistent volume and a non-shared persistent
// volume. We have a similar test in linux_filesystem_isolator_tests.cpp
// which tests the implementation of `filesystem/linux` isolator, and
// this one tests the implementation of `filesystem/posix` isolator.
TEST_P(PersistentVolumeTest, UNPRIVILEGED_USER_PersistentVolumes)
{
  // Reinitialize libprocess to ensure volume gid manager's metrics
  // can be added in each iteration of this test (i.e., run this test
  // repeatedly with the `--gtest_repeat` option).
  process::reinitialize(None(), None(), None());

  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.volume_gid_range = "[10000-20000]";

  // Agent's work directory and `diskPath` are created with the
  // mode 0700, here we change their modes to 0711 to ensure the
  // non-root user used to launch the command task can enter it.
  ASSERT_SOME(os::chmod(slaveFlags.work_dir, 0711));
  ASSERT_SOME(os::chmod(diskPath, 0711));

  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  // Create two persistent volumes (shared and non-shared),
  // and launch a task to write a file to each volume.
  Resource volume1 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 1),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  Resource volume2 = createPersistentVolume(
      getDiskResource(Megabytes(2048), 2),
      "id2",
      "path2",
      None(),
      frameworkInfo.principal(),
      false); // Non-shared volume.

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  CommandInfo command = createCommandInfo(
        "echo hello > path1/file && echo world > path2/file");

  command.set_user(user.get());

  TaskInfo task = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:1;mem:128").get() + volume1 + volume2,
      command);

  // We should receive a TASK_STARTING, a TASK_RUNNING
  // and a TASK_FINISHED for the launched task.
  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume1),
       CREATE(volume2),
       LAUNCH({task})});

  AWAIT_READY(status0);
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  // One gid should have been allocated to the volume. Please note that shared
  // persistent volume's gid will be deallocated only when it is destroyed.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 2,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());

  // Resume the clock so the terminating task and executor can be reaped.
  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the master recovers after a failover and
// re-offers the shared persistent volume when tasks using the same
// volume are still running.
TEST_P(PersistentVolumeTest, SharedPersistentVolumeMasterFailover)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage1);

  // Create the framework with SHARED_RESOURCES capability.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers1->empty());

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

  // We should receive a TASK_STARTING and a TASK_RUNNING for each of the tasks.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4));

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume),
       LAUNCH({task1, task2})});

  AWAIT_READY(message);

  // We only check the first and the last status, because the two in between
  // could arrive in any order.
  AWAIT_READY(status1);
  EXPECT_EQ(TASK_STARTING, status1->state());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_RUNNING, status4->state());

  // This is to make sure operation message is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate failed over master by restarting the master.
  master->reset();

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<UpdateSlaveMessage> updateSlaveMessage2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(updateSlaveMessage2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  // Verify the offer from the failed over master.
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(
          Resources::parse("cpus:1;mem:1024").get(),
          frameworkInfo.roles(0))));
  EXPECT_TRUE(Resources(offer2.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  ASSERT_FALSE(offers->empty());

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

  // We should receive a TASK_STARTING and a TASK_RUNNING each of the 2 tasks.
  // We track task termination by a TASK_FINISHED for the short-lived task.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;
  Future<TaskStatus> status5;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4))
    .WillOnce(FutureArg<1>(&status5));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(sharedVolume),
       CREATE(nonSharedVolume),
       LAUNCH({task1, task2})},
      filters);

  // Wait for TASK_STARTING and TASK_RUNNING for both the tasks,
  // and TASK_FINISHED for the short-lived task.
  AWAIT_READY(status1);
  AWAIT_READY(status2);
  AWAIT_READY(status3);
  AWAIT_READY(status4);
  AWAIT_READY(status5);

  hashset<TaskID> tasksRunning;
  hashset<TaskID> tasksFinished;
  vector<Future<TaskStatus>> statuses {
    status1, status2, status3, status4, status5};

  foreach (const Future<TaskStatus>& status, statuses) {
    if (status->state() == TASK_STARTING) {
      // ignore
    } else if (status->state() == TASK_RUNNING) {
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
  ASSERT_FALSE(offers->empty());

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
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the shared persistent volume is in the offer, but the
  // non-shared volume is not in the offer.
  EXPECT_TRUE(unallocated(offer.resources()).contains(sharedVolume));
  EXPECT_FALSE(unallocated(offer.resources()).contains(nonSharedVolume));

  // We kill the long-lived task and wait for TASK_KILLED, so we can
  // DESTROY the persistent volume once the task terminates.
  Future<TaskStatus> status6;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status6));

  driver.killTask(task1.task_id());

  AWAIT_READY(status6);
  EXPECT_EQ(task1.task_id(), status6->task_id());
  EXPECT_EQ(TASK_KILLED, status6->state());

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
  ASSERT_FALSE(offers->empty());

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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  // 1. Create framework so that all resources are offered to this framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers->empty());

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

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
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
  ASSERT_FALSE(offers->empty());

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

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Future<Nothing> ack1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ack2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack1);

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack2);

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

  // Wait for the slave to reregister.
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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      frameworkInfo.principal());

  Future<Resources> message1 = getOperationMessage(slave.get()->pid);

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the operation message response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(message1);
  EXPECT_TRUE(message1->contains(volume));

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was created successfully.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<Resources> message2 = getOperationMessage(slave.get()->pid);

  // Destroy the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  AWAIT_READY(message2);
  EXPECT_TRUE(message2->contains(volume));

  // Expect an offer that does not contain the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is not in the offer.
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

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
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_principal();
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master. Since the framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo.roles(0);
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave. Resources are being statically reserved because persistent
  // volume creation requires reserved resources.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      getDiskResource(Megabytes(2048)),
      "id1",
      "path1",
      None(),
      None());

  Future<Resources> message1 = getOperationMessage(slave.get()->pid);

  // Create the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume)},
      filters);

  // Await the operation message response after the volume is created
  // and check that it contains the volume.
  AWAIT_READY(message1);
  EXPECT_TRUE(message1->contains(volume));

  // Expect an offer containing the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await the offer containing the persistent volume.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was successfully created.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));
  EXPECT_TRUE(os::exists(slave::paths::getPersistentVolumePath(
      slaveFlags.work_dir,
      volume)));

  Future<Resources> message2 = getOperationMessage(slave.get()->pid);

  // Destroy the persistent volume using `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {DESTROY(volume)},
      filters);

  AWAIT_READY(message2);

  // Expect an offer that does not contain the persistent volume.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume was not created.
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));
  EXPECT_TRUE(message2->contains(volume));

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
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.clear_principal();
  frameworkInfo1.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a `FrameworkInfo` with a principal.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a master. Since one framework has no
  // principal, we don't authenticate frameworks.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.roles(0);
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  ASSERT_FALSE(offers->empty());

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
    ASSERT_FALSE(offers->empty());

    offer = offers.get()[0];

    // Check that the persistent volume is not contained in this offer.
    EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.roles(0))));
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
  ASSERT_FALSE(offers->empty());

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
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo2.roles(0))));

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
  ASSERT_FALSE(offers->empty());

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
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is still contained in this offer.
  // TODO(greggomann): In addition to checking that the volume is contained in
  // the offer, we should also confirm that the Destroy operation failed for the
  // correct reason. See MESOS-5470.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.roles(0))));

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
  frameworkInfo1.set_roles(0, DEFAULT_TEST_ROLE);

  // Create a `FrameworkInfo` that can create volumes.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("creator-framework");
  frameworkInfo2.set_user(os::user().get());
  frameworkInfo2.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo2.set_principal("creator-principal");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.roles = frameworkInfo1.roles(0);
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = getSlaveResources();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  ASSERT_FALSE(offers->empty());

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
    ASSERT_FALSE(offers->empty());

    offer = offers.get()[0];

    // Check that the persistent volume is not contained in this offer.
    EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.roles(0))));
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
  ASSERT_FALSE(offers->empty());

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
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is contained in this offer.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo2.roles(0))));

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
  ASSERT_FALSE(offers->empty());

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
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  // Check that the persistent volume is still contained in this offer.
  // TODO(greggomann): In addition to checking that the volume is contained in
  // the offer, we should also confirm that the Destroy operation failed for the
  // correct reason. See MESOS-5470.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo1.roles(0))));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
