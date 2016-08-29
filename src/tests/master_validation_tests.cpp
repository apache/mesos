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

#include <google/protobuf/repeated_field.h>

#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "master/master.hpp"
#include "master/validation.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos::internal::master::validation;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class ResourceValidationTest : public ::testing::Test
{
protected:
  RepeatedPtrField<Resource> CreateResources(const Resource& resource)
  {
    RepeatedPtrField<Resource> resources;
    resources.Add()->CopyFrom(resource);
    return resources;
  }
};


TEST_F(ResourceValidationTest, StaticReservation)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  EXPECT_NONE(resource::validate(CreateResources(resource)));
}


TEST_F(ResourceValidationTest, DynamicReservation)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  EXPECT_NONE(resource::validate(CreateResources(resource)));
}


TEST_F(ResourceValidationTest, RevocableDynamicReservation)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));
  resource.mutable_revocable();

  EXPECT_SOME(resource::validate(CreateResources(resource)));
}


TEST_F(ResourceValidationTest, InvalidRoleReservationPair)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  EXPECT_SOME(resource::validate(CreateResources(resource)));
}


TEST_F(ResourceValidationTest, PersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_NONE(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, UnreservedDiskInfo)
{
  Resource volume = Resources::parse("disk", "128", "*").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, InvalidPersistenceID)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1/", "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithoutVolumeInfo)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", None()));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, ReadOnlyPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1", Volume::RO));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithHostPath)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(
      createDiskInfo("id1", "path1", Volume::RW, "foo"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, NonPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo(None(), "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, RevocablePersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
  volume.mutable_revocable();

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, UnshareableResource)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_shared();

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, SharedPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
  volume.mutable_shared();

  EXPECT_NONE(resource::validate(CreateResources(volume)));
}


class ReserveOperationValidationTest : public MesosTest {};


// This test verifies that the 'role' specified in the resources of
// the RESERVE operation needs to match the framework's 'role'.
TEST_F(ReserveOperationValidationTest, MatchingRole)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_NONE(operation::validate(reserve, "principal"));
}


// This test verifies that validation fails if the framework has a
// "*" role even if the role matches.
TEST_F(ReserveOperationValidationTest, DisallowStarRoleFrameworks)
{
  // The role "*" matches, but is invalid since frameworks with
  // "*" role cannot reserve resources.
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_SOME(operation::validate(reserve, "principal"));
}


// This test verifies that validation fails if the framework attempts
// to reserve for the "*" role.
TEST_F(ReserveOperationValidationTest, DisallowReserveForStarRole)
{
  // Principal "principal" reserving for "*".
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.mutable_reservation()->CopyFrom(
      createReservationInfo("principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_SOME(operation::validate(reserve, "principal"));
}


// This test verifies that the 'principal' specified in the resources
// of the RESERVE operation needs to match the framework's 'principal'.
TEST_F(ReserveOperationValidationTest, MatchingPrincipal)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_NONE(operation::validate(reserve, "principal"));
}


// This test verifies that validation fails if the 'principal'
// specified in the resources of the RESERVE operation do not match
// the framework's 'principal'.
TEST_F(ReserveOperationValidationTest, NonMatchingPrincipal)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal2"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_SOME(operation::validate(reserve, "principal1"));
}


// This test verifies that validation fails if the `principal`
// in `ReservationInfo` is not set.
TEST_F(ReserveOperationValidationTest, ReservationInfoMissingPrincipal)
{
  Resource::ReservationInfo reservationInfo;

  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(reservationInfo);

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  EXPECT_SOME(operation::validate(reserve, "principal"));
}


// This test verifies that validation fails if there are statically
// reserved resources specified in the RESERVE operation.
TEST_F(ReserveOperationValidationTest, StaticReservation)
{
  Resource staticallyReserved = Resources::parse("cpus", "8", "role").get();

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(staticallyReserved);

  EXPECT_SOME(operation::validate(reserve, "principal"));
}


// This test verifies that the resources specified in the RESERVE
// operation cannot be persistent volumes.
TEST_F(ReserveOperationValidationTest, NoPersistentVolumes)
{
  Resource reserved = Resources::parse("cpus", "8", "role").get();
  reserved.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(reserved);

  EXPECT_NONE(operation::validate(reserve, "principal"));
}


// This test verifies that validation fails if there are persistent
// volumes specified in the resources of the RESERVE operation.
TEST_F(ReserveOperationValidationTest, PersistentVolumes)
{
  Resource reserved = Resources::parse("cpus", "8", "role").get();
  reserved.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Resource volume = Resources::parse("disk", "128", "role").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(reserved);
  reserve.add_resources()->CopyFrom(volume);

  EXPECT_SOME(operation::validate(reserve, "principal"));
}


class UnreserveOperationValidationTest : public MesosTest {};


// This test verifies that any resources can be unreserved by any
// framework with a principal.
TEST_F(UnreserveOperationValidationTest, WithoutACL)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(resource);

  EXPECT_NONE(operation::validate(unreserve));
}


// This test verifies that validation succeeds if the framework's
// `principal` is not set.
TEST_F(UnreserveOperationValidationTest, FrameworkMissingPrincipal)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();
  resource.mutable_reservation()->CopyFrom(createReservationInfo());

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(resource);

  EXPECT_NONE(operation::validate(unreserve));
}


// This test verifies that validation fails if there are statically
// reserved resources specified in the UNRESERVE operation.
TEST_F(UnreserveOperationValidationTest, StaticReservation)
{
  Resource staticallyReserved = Resources::parse("cpus", "8", "role").get();

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(staticallyReserved);

  EXPECT_SOME(operation::validate(unreserve));
}


// This test verifies that the resources specified in the UNRESERVE
// operation cannot be persistent volumes.
TEST_F(UnreserveOperationValidationTest, NoPersistentVolumes)
{
  Resource reserved = Resources::parse("cpus", "8", "role").get();
  reserved.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(reserved);

  EXPECT_NONE(operation::validate(unreserve));
}


// This test verifies that validation fails if there are persistent
// volumes specified in the resources of the UNRESERVE operation.
TEST_F(UnreserveOperationValidationTest, PersistentVolumes)
{
  Resource reserved = Resources::parse("cpus", "8", "role").get();
  reserved.mutable_reservation()->CopyFrom(createReservationInfo("principal"));

  Resource volume = Resources::parse("disk", "128", "role").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(reserved);
  unreserve.add_resources()->CopyFrom(volume);

  EXPECT_SOME(operation::validate(unreserve));
}


class CreateOperationValidationTest : public MesosTest {};


// This test verifies that all resources specified in the CREATE
// operation are persistent volumes.
TEST_F(CreateOperationValidationTest, PersistentVolumes)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  EXPECT_NONE(operation::validate(create, Resources(), None()));

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  create.add_volumes()->CopyFrom(cpus);

  EXPECT_SOME(operation::validate(create, Resources(), None()));
}


TEST_F(CreateOperationValidationTest, DuplicatedPersistenceID)
{
  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume1);

  EXPECT_NONE(operation::validate(create, Resources(), None()));

  Resource volume2 = Resources::parse("disk", "64", "role1").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_SOME(operation::validate(create, volume1, None()));

  create.add_volumes()->CopyFrom(volume2);

  EXPECT_SOME(operation::validate(create, Resources(), None()));
}


// This test confirms that Create operations will be invalidated if they contain
// a principal in `DiskInfo` that does not match the principal of the framework
// or operator performing the operation.
TEST_F(CreateOperationValidationTest, NonMatchingPrincipal)
{
  // An operation with an incorrect principal in `DiskInfo.Persistence`.
  {
    Resource volume = Resources::parse("disk", "128", "role1").get();
    volume.mutable_disk()->CopyFrom(
        createDiskInfo("id1", "path1", None(), None(), None(), "principal"));

    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(volume);

    EXPECT_SOME(operation::validate(create, Resources(), "other-principal"));
  }

  // An operation without a principal in `DiskInfo.Persistence`.
  {
    Resource volume = Resources::parse("disk", "128", "role1").get();
    volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(volume);

    EXPECT_SOME(operation::validate(create, Resources(), "principal"));
  }
}


// This test verifies that creating a persistent volume that is larger
// than the offered disk resource results won't succeed.
TEST_F(CreateOperationValidationTest, InsufficientDiskResource)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  master::Flags masterFlags = CreateMasterFlags();

  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values(frameworkInfo.principal());
  acl->mutable_roles()->add_values(frameworkInfo.role());

  masterFlags.acls = acls;
  masterFlags.roles = "role1";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_FALSE(offers1.get().empty());

  Offer offer1 = offers1.get()[0];

  // Since the CREATE operation will fail, we don't expect any
  // CheckpointResourcesMessage to be sent.
  EXPECT_NO_FUTURE_PROTOBUFS(CheckpointResourcesMessage(), _, _);

  Resources volume = createPersistentVolume(
      Megabytes(2048),
      "role1",
      "id1",
      "path1");

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume)},
      filters);

  // Advance the clock to trigger another allocation.
  Clock::pause();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2.get().empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_EQ(Resources(offer1.resources()), Resources(offer2.resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


class DestroyOperationValidationTest : public ::testing::Test {};


// This test verifies that all resources specified in the DESTROY
// operation are persistent volumes.
TEST_F(DestroyOperationValidationTest, PersistentVolumes)
{
  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Resource volume2 = Resources::parse("disk", "64", "role1").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id2", "path2"));

  Resources volumes;
  volumes += volume1;
  volumes += volume2;

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(volume1);

  EXPECT_NONE(operation::validate(destroy, volumes));

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  destroy.add_volumes()->CopyFrom(cpus);

  EXPECT_SOME(operation::validate(destroy, volumes));
}


TEST_F(DestroyOperationValidationTest, UnknownPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(volume);

  EXPECT_NONE(operation::validate(destroy, volume));
  EXPECT_SOME(operation::validate(destroy, Resources()));
}


// TODO(jieyu): All of the task validation tests have the same flow:
// launch a task, expect an update of a particular format (invalid w/
// message). Consider providing common functionalities in the test
// fixture to avoid code bloat. Ultimately, we should make task or
// offer validation unit testable.
class TaskValidationTest : public MesosTest {};


TEST_F(TaskValidationTest, ExecutorUsesInvalidFrameworkID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor with a random framework id.
  ExecutorInfo executor;
  executor = DEFAULT_EXECUTOR_INFO;
  executor.mutable_framework_id()->set_value(UUID::random().toString());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_TRUE(strings::startsWith(
      status.get().message(), "ExecutorInfo has an invalid FrameworkID"));

  driver.stop();
  driver.join();
}


// The master should fill in the `ExecutorInfo.framework_id`
// if it is not set by the framework.
TEST_F(TaskValidationTest, ExecutorMissingFrameworkID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the first slave.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor with a missing framework id.
  ExecutorInfo executor;
  executor = DEFAULT_EXECUTOR_INFO;
  executor.clear_framework_id();

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  // The task should pass validation since the framework id
  // is filled in, and when it reaches the dummy executor
  // it will fail because the executor just exits.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesCommandInfoAndExecutorInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create a task that uses both command info and task info.
  TaskInfo task = createTask(offers.get()[0], ""); // Command task.
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO); // Executor task.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_TRUE(strings::contains(
      status.get().message(), "CommandInfo or ExecutorInfo present"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesExecutorInfoWithoutCommandInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor without command info.
  // Note that we don't set type as 'CUSTOM' because it is not
  // required for `LAUNCH` operation.
  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::startsWith(
      status->message(), "'ExecutorInfo.command' must be set"));

  driver.stop();
  driver.join();
}


// This test verifies that a scheduler cannot explicitly specify
// a 'DEFAULT' executor when using `LAUNCH` operation.
// TODO(vinod): Revisit this when the above is allowed.
TEST_F(TaskValidationTest, TaskUsesDefaultExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create a 'DEFAULT' executor.
  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("default");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::startsWith(
      status->message(), "'ExecutorInfo.type' must be 'CUSTOM'"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesNoResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status.get().reason());
  EXPECT_TRUE(status.get().has_message());
  EXPECT_EQ("Task uses no resources", status.get().message());

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesMoreResourcesThanOffered)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(2.01);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  EXPECT_EQ(task.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status.get().reason());
  EXPECT_TRUE(status.get().has_message());
  EXPECT_TRUE(strings::contains(
      status.get().message(), "more than available"));

  driver.stop();
  driver.join();
}


// This test verifies that if two tasks are launched with the same
// task ID, the second task will get rejected.
TEST_F(TaskValidationTest, DuplicatedTaskID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  // Create two tasks with the same id.
  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:32").get());
  task1.mutable_executor()->MergeFrom(executor);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("1");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:32").get());
  task2.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Grab the first task but don't send a status update.
  Future<TaskInfo> task;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&task));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(task);
  EXPECT_EQ(task1.task_id(), task.get().task_id());

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status.get().reason());

  EXPECT_TRUE(strings::startsWith(
      status.get().message(), "Task has duplicate ID"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that two tasks launched on the same slave with
// the same executor id but different executor info are rejected.
TEST_F(TaskValidationTest, ExecutorInfoDiffersOnSameSlave)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor);

  executor.mutable_command()->set_value("exit 2");

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task2.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  // Grab the "good" task but don't send a status update.
  Future<TaskInfo> task;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&task));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(task);
  EXPECT_EQ(task1.task_id(), task.get().task_id());

  AWAIT_READY(status);
  EXPECT_EQ(task2.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status.get().reason());
  EXPECT_TRUE(status.get().has_message());
  EXPECT_TRUE(strings::contains(
      status.get().message(), "ExecutorInfo is not compatible"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that two tasks each launched on a different
// slave with same executor id but different executor info are
// allowed.
TEST_F(TaskValidationTest, ExecutorInfoDiffersOnDifferentSlaves)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Start the first slave.
  MockExecutor exec1(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer1(&exec1);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer1);
  ASSERT_SOME(slave1);

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Launch the first task with the default executor id.
  ExecutorInfo executor1;
  executor1 = DEFAULT_EXECUTOR_INFO;
  executor1.mutable_command()->set_value("exit 1");

  TaskInfo task1 = createTask(
      offers1.get()[0], executor1.command().value(), executor1.executor_id());

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1.get().state());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Now start the second slave.
  MockExecutor exec2(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer2(&exec2);

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Now launch the second task with the same executor id but
  // a different executor command.
  ExecutorInfo executor2;
  executor2 = executor1;
  executor2.mutable_command()->set_value("exit 2");

  TaskInfo task2 = createTask(
      offers2.get()[0], executor2.command().value(), executor2.executor_id());

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a task is not allowed to mix revocable and
// non-revocable resources.
TEST_F(TaskValidationTest, TaskUsesRevocableResources)
{
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("task");
  task.mutable_slave_id()->set_value("slave");

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);
  EXPECT_NONE(task::internal::validateResources(task));

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);
  EXPECT_NONE(task::internal::validateResources(task));

  // A task with both revocable and non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);
  task.add_resources()->CopyFrom(revocableCpus);
  EXPECT_SOME(task::internal::validateResources(task));
}


// This test verifies that a task and its executor are not allowed to
// mix revocable and non-revocable resources.
TEST_F(TaskValidationTest, TaskAndExecutorUseRevocableResources)
{
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("task");
  task.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task and executor with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);
  executor.add_resources()->CopyFrom(cpus);
  task.mutable_executor()->CopyFrom(executor);
  EXPECT_NONE(task::internal::validateTaskAndExecutorResources(task));

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task and executor with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);
  task.mutable_executor()->CopyFrom(executor);
  EXPECT_NONE(task::internal::validateTaskAndExecutorResources(task));

  // A task with revocable cpus and its executor with non-revocable
  // cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(cpus);
  task.mutable_executor()->CopyFrom(executor);
  EXPECT_SOME(task::internal::validateTaskAndExecutorResources(task));

  // A task with non-revocable cpus and its executor with
  // non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);
  task.mutable_executor()->CopyFrom(executor);
  EXPECT_SOME(task::internal::validateTaskAndExecutorResources(task));
}


// Ensures that negative executor shutdown grace period in `ExecutorInfo`
// is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, ExecutorShutdownGracePeriodIsNonNegative)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());
  Offer offer = offers.get()[0];

  ExecutorInfo executorInfo(DEFAULT_EXECUTOR_INFO);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(-1).ns());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(executorInfo);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_EQ("ExecutorInfo's 'shutdown_grace_period' must be non-negative",
            status->message());

  driver.stop();
  driver.join();
}


// Ensures that negative grace period in `KillPolicy`
// is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, KillPolicyGracePeriodIsNonNegative)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_kill_policy()->mutable_grace_period()->set_nanoseconds(
      Seconds(-1).ns());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_EQ("Task's 'kill_policy.grace_period' must be non-negative",
            status->message());

  driver.stop();
  driver.join();
}

// TODO(jieyu): Add tests for checking duplicated persistence ID
// against offered resources.

// TODO(jieyu): Add tests for checking duplicated persistence ID
// across task and executors.

// TODO(jieyu): Add tests for checking duplicated persistence ID
// within an executor.

// TODO(benh): Add tests for checking correct slave IDs.

// TODO(benh): Add tests for checking executor resource usage.

// TODO(benh): Add tests which launch multiple tasks and check for
// aggregate resource usage.


class ExecutorValidationTest : public MesosTest {};


TEST_F(ExecutorValidationTest, ExecutorType)
{
  ExecutorInfo executorInfo;
  executorInfo = DEFAULT_EXECUTOR_INFO;
  executorInfo.mutable_framework_id()->set_value(UUID::random().toString());

  {
    // 'CUSTOM' executor with `CommandInfo` set is valid.
    executorInfo.set_type(ExecutorInfo::CUSTOM);
    executorInfo.mutable_command();

    EXPECT_NONE(::executor::internal::validateType(executorInfo));
  }

  {
    // 'CUSTOM' executor without `CommandInfo` set is invalid.
    executorInfo.set_type(ExecutorInfo::CUSTOM);
    executorInfo.clear_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);
    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.command' must be set for 'CUSTOM' executor"));
  }

  {
    // 'DEFAULT' executor without `CommandInfo` set is valid.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.clear_command();

    EXPECT_NONE(::executor::internal::validateType(executorInfo));
  }

  {
    // 'DEFAULT' executor with `CommandInfo` set is invalid.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.mutable_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);
    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.command' must not be set for 'DEFAULT' executor"));
  }
}


class TaskGroupValidationTest : public MesosTest {};


// This test verifies that tasks in a task group cannot mix
// revocable and non-revocable resources.
TEST_F(TaskGroupValidationTest, TaskGroupUsesRevocableResources)
{
  TaskInfo task1;
  task1.set_name("test1");
  task1.mutable_task_id()->set_value("task1");
  task1.mutable_slave_id()->set_value("slave");

  TaskInfo task2;
  task2.set_name("test2");
  task2.mutable_task_id()->set_value("task2");
  task2.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task group with only non-revocable cpus is valid.
  task1.add_resources()->CopyFrom(cpus);
  task2.add_resources()->CopyFrom(cpus);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  EXPECT_NONE(task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor));

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task group with only revocable cpus is valid.
  task1.clear_resources();
  task2.clear_resources();
  task1.add_resources()->CopyFrom(revocableCpus);
  task2.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  EXPECT_NONE(task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor));

  // A task group with one task using revocable resources and another task
  // using non-revocable cpus is invalid.
  task1.clear_resources();
  task2.clear_resources();
  task1.add_resources()->CopyFrom(cpus);
  task2.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  EXPECT_SOME(task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor));
}


// This test verifies that tasks in a task group and executor
// cannot mix revocable and non-revocable resources.
TEST_F(TaskGroupValidationTest, TaskGroupAndExecutorUsesRevocableResources)
{
  TaskInfo task;
  task.set_name("test1");
  task.mutable_task_id()->set_value("task1");
  task.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task group and executor with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  executor.add_resources()->CopyFrom(cpus);

  EXPECT_NONE(task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor));

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task group and executor with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);

  EXPECT_NONE(task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor));

  // A task group with the task using revocable resources and executor
  // using non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(cpus);

  Option<Error> error =
    task::group::internal::validateTaskGroupAndExecutorResources(
        taskGroup, executor);

  EXPECT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "Task group and executor mix revocable and non-revocable resources"));

  // A task group with the task using non-revocable resources and executor
  // using revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "Task group and executor mix revocable and non-revocable resources"));
}


// Ensures that an executor without a framework id is
// rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, ExecutorWithoutFrameworkId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Create an invalid executor without framework id.
  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("E");

  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(offers.get()[0].resources());

  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(offers.get()[0].resources());

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());

  driver.stop();
  driver.join();
}


// This test verifies that a task group task
// that has `NetworkInfo` set is invalid.
TEST_F(TaskGroupValidationTest, TaskUsesNetworkInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());
  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:1;mem:512;disk:32").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_resources()->CopyFrom(resources);

  // Create a task that has NetworkInfos set.
  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(resources);
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->add_network_infos();

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, status->reason());
  EXPECT_EQ("Task '1' is invalid: NetworkInfos must not be set on the task",
            status->message());

  driver.stop();
  driver.join();
}


// Ensures that a task in a task group with an executor
// is rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, TaskUsesExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());
  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:1;mem:512;disk:32").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_resources()->CopyFrom(resources);

  // Create an invalid task that has executor.
  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offer.slave_id());
  task1.mutable_resources()->MergeFrom(resources);
  task1.mutable_executor()->MergeFrom(executor);

  // Create a valid task.
  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offer.slave_id());
  task1.mutable_resources()->MergeFrom(resources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(task1.task_id(), task1Status->task_id());
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ("Task '1' is invalid: 'TaskInfo.executor' must not be set",
            task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(task2.task_id(), task2Status->task_id());
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
