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

#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/allocator/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/some.hpp>
#include <stout/strings.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/detector/standalone.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::DoDefault;

namespace mesos {
namespace internal {
namespace tests {

class ReservationTest : public MesosTest {};


// This tests that a framework can send back a Reserve offer operation
// as a response to an offer, which updates the resources in the
// allocator and results in the reserved resources being reoffered to
// the framework. The framework then sends back an Unreserved offer
// operation to unreserve the reserved resources. Finally, We test
// that the framework receives the unreserved resources.
TEST_F(ReservationTest, ReserveThenUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This tests for failures arising from floating point precision errors during
// processing of resource reservation requests.  The test first asks a framework
// to send a resource reservation request in response to an offer, which updates
// the resources in the allocator and results in resources being re-offered to
// the framework. The framework then sends back a new resource reservation
// request which involves a floating point value for the resources being
// reserved, which in turn triggers a problematic floating point comparison.
TEST_F(ReservationTest, ReserveTwiceWithDoubleValue)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:24;mem:4096";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered (default would be 5 seconods).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:0.1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.flatten(
        frameworkInfo.role(),
        createReservationInfo(frameworkInfo.principal())).get();

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
     .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  // In the first offer, expect an offer with unreserved resources.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
     .WillOnce(FutureArg<1>(&offers));

  // First iteration: Reserving 0.1 CPU.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // In the second offer, expect an offer with reserved resources.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
     .WillOnce(FutureArg<1>(&offers));

  // Second iteration: Reserving second 0.1 CPU.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // The agent should be able to calculate the remaining resources
  // correctly at this point. If the floating point comparison on the
  // agent isn't correct it will end up failing `CHECKS` on the agent,
  // potentially crashing the agent. See MESOS-3552.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  Resources reserved = Resources::parse("cpus:0.2;mem:512").get();
  Resources finalReservation =
    reserved.flatten(
        frameworkInfo.role(),
        createReservationInfo(frameworkInfo.principal())).get();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(finalReservation, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This tests that a framework can send back a Reserve followed by a
// LaunchTasks offer operation as a response to an offer, which
// updates the resources in the allocator then proceeds to launch the
// task with the reserved resources. The reserved resources are
// reoffered to the framework on task completion. The framework then
// sends back an Unreserved offer operation to unreserve the reserved
// resources. We test that the framework receives the unreserved
// resources.
TEST_F(ReservationTest, ReserveAndLaunchThenUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // Create a task.
  TaskInfo taskInfo =
    createTask(offer.slave_id(), dynamicallyReserved, "exit 1", exec.id);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoDefault());

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources and launch the tasks.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReserved),
      LAUNCH({taskInfo})});

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test launches 2 frameworks in the same role. framework1
// reserves resources by sending back a Reserve offer operation. We
// first test that framework1 receives the reserved resources, then on
// the next resource offer, framework1 declines the offer. This
// should lead to framework2 receiving the resources that framework1
// reserved.
TEST_F(ReservationTest, ReserveShareWithinRole)
{
  string role = "role";

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_role(role);

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework2");
  frameworkInfo2.set_role(role);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = role;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      role, createReservationInfo(frameworkInfo1.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo1.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver1.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.role())));

  // The filter to decline the offer "forever".
  Filters filtersForever;
  filtersForever.set_refuse_seconds(std::numeric_limits<double>::max());

  // Decline the offer "forever" in order to force framework2 to
  // receive the resources.
  driver1.declineOffer(offer.id(), filtersForever);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // The expectation for the next offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // In the next offer, expect an offer with the resources reserved by
  // framework1.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.role())));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This tests that a Reserve offer operation where the specified
// resources does not exist in the given offer (too large, in this
// case) is dropped.
TEST_F(ReservationTest, DropReserveTooLarge)
{
  TestAllocator<> allocator;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources unreservedTooLarge = Resources::parse("cpus:1;mem:1024").get();
  Resources dynamicallyReservedTooLarge = unreservedTooLarge.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture the offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve resources that are too large for the offer.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReservedTooLarge)},
      filters);

  // In the next offer, still expect an offer with the unreserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This tests that an attempt to dynamically reserve statically
// reserved resources is dropped.
TEST_F(ReservationTest, DropReserveStaticReservation)
{
  TestAllocator<> allocator;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus(role):1;mem(role):512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources staticallyReserved =
    Resources::parse("cpus(role):1;mem(role):512").get();
  Resources dynamicallyReserved = staticallyReserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  driver.start();

  // In the first offer, expect an offer with the statically reserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(staticallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve the statically reserved resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, still expect an offer with the statically
  // reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(staticallyReserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that CheckpointResourcesMessages are sent to the
// slave when a framework reserve/unreserves resources, and the
// resources in the messages correctly reflect the resources that need
// to be checkpointed on the slave.
TEST_F(ReservationTest, SendingCheckpointResourcesMessage)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved1 = Resources::parse("cpus:8").get();
  Resources reserved1 = unreserved1.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  Resources unreserved2 = Resources::parse("mem:2048").get();
  Resources reserved2 = unreserved2.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // In the first offer, expect the sum of 'unreserved1' and
  // 'unreserved2'.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved1 + unreserved2, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> message3 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve and unreserve resources.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(reserved1), RESERVE(reserved2), UNRESERVE(reserved1)},
      filters);

  // NOTE: Currently, we send one message per operation. But this is
  // an implementation detail which is subject to change.

  // Expect the 'RESERVE(reserved1)' as the first message.
  // The checkpointed resources should correspond to 'reserved1'.
  AWAIT_READY(message1);
  EXPECT_EQ(Resources(message1->resources()), reserved1);

  // Expect the 'RESERVE(reserved2)' as the second message.
  // The checkpointed resources should correspond to
  // 'reserved1 + reserved2'.
  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2->resources()), reserved1 + reserved2);

  // Expect the 'UNRESERVE(reserved1)' as the third message.
  // The checkpointed resources should correspond to 'reserved2'.
  AWAIT_READY(message3);
  EXPECT_EQ(Resources(message3->resources()), reserved2);

  driver.stop();
  driver.join();
}


// This test verifies that the slave checkpoints the resources for
// dynamic reservations to the disk, recovers them upon restart, and
// sends them to the master during re-registration.
TEST_F(ReservationTest, ResourcesCheckpointing)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.recover = "reconnect";
  slaveFlags.resources = "cpus:8;mem:4096";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the 'CheckpointResourcesMessage'.
  AWAIT_READY(checkpointResources);

  // Restart the slave without shutting down.
  slave.get()->terminate();

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  Future<Nothing> slaveRecover = FUTURE_DISPATCH(_, &Slave::recover);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Expect the slave to recover.
  AWAIT_READY(slaveRecover);

  // Expect to receive the 'ReregisterSlaveMessage' containing the
  // reserved resources.
  AWAIT_READY(reregisterSlave);

  EXPECT_EQ(reregisterSlave->checkpointed_resources(), reserved);

  driver.stop();
  driver.join();
}


// This test verifies the case where a slave that has checkpointed
// dynamic reservations reregisters with a failed over master, and the
// dynamic reservations are later correctly offered to the framework.
TEST_F(ReservationTest, MasterFailover)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master1 = StartMaster(masterFlags);
  ASSERT_SOME(master1);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:2048";

  StandaloneMasterDetector detector(master1.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get()->pid);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the CheckpointResourcesMessage.
  AWAIT_READY(checkpointResources);

  // This is to make sure CheckpointResourcesMessage is processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate master failover by restarting the master.
  master1->reset();

  Try<Owned<cluster::Master>> master2 = StartMaster(masterFlags);
  ASSERT_SOME(master2);

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master2.get()->pid);

  // Ensure agent registration is processed.
  Clock::pause();
  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  Clock::resume();

  // Wait for slave to confirm re-registration.
  AWAIT_READY(slaveReregistered);

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// NOTE: The following tests covers the notion of compatible resources
// on slave restart. Newly declared resources are compatible if they
// include the checkpointed resources. For example, suppose a slave
// initially declares "cpus:8;mem:4096", and "cpus:8;mem:2048" gets
// reserved and thus checkpointed. In order to be compatible, the
// newly declared resources must include "cpus:8;mem:2048".  For
// example, "cpus:12;mem:2048" would be considered compatible.


// This test verifies that a slave can restart as long as the
// checkpointed resources it recovers are compatible with the slave
// resources specified using the '--resources' flag.
TEST_F(ReservationTest, CompatibleCheckpointedResources)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Wait until CheckpointResourcesMessage arrives.
  AWAIT_READY(checkpointResources);

  terminate(slave1);
  wait(slave1);

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it is compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:12;mem:2048";

  MockSlave slave2(slaveFlags, &detector, &containerizer);

  Future<Future<Nothing>> recover;
  EXPECT_CALL(slave2, __recover(_))
    .WillOnce(DoAll(FutureArg<0>(&recover), Return()));

  spawn(slave2);

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect 'recover' to have completed successfully.
  AWAIT_READY(recover.get());

  terminate(slave2);
  wait(slave2);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a slave can restart as long as the
// checkpointed resources (including persistent volumes) it recovers
// are compatible with the slave resources specified using the
// '--resources' flag.
TEST_F(ReservationTest, CompatibleCheckpointedResourcesWithPersistentVolumes)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096;disk:2048";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  Resource unreservedDisk = Resources::parse("disk", "1024", "*").get();
  Resource reservedDisk = unreservedDisk;
  reservedDisk.set_role(frameworkInfo.role());
  reservedDisk.mutable_reservation()->CopyFrom(
      createReservationInfo(frameworkInfo.principal()));

  Resource volume = reservedDisk;
  volume.mutable_disk()->CopyFrom(createDiskInfo(
      "persistence_id",
      "container_path",
      None(),
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved + unreservedDisk, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> message2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Reserve the resources and create the volume.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(reserved + reservedDisk), CREATE(volume)},
      filters);

  // NOTE: Currently, we send one message per operation. But this is
  // an implementation detail which is subject to change.
  AWAIT_READY(message1);
  EXPECT_EQ(Resources(message1->resources()), reserved + reservedDisk);

  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2->resources()), reserved + volume);

  // Expect an offer containing the volume.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved + volume, frameworkInfo.role())));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  terminate(slave1);
  wait(slave1);

  AWAIT_READY(rescindedOfferId);
  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it is compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:12;mem:2048;disk:1024";

  MockSlave slave2(slaveFlags, &detector, &containerizer);

  Future<Future<Nothing>> recover;
  EXPECT_CALL(slave2, __recover(_))
    .WillOnce(DoAll(FutureArg<0>(&recover), Return()));

  spawn(slave2);

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect that 'recover' will complete successfully.
  AWAIT_READY(recover.get());

  terminate(slave2);
  wait(slave2);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a slave will refuse to start if the
// checkpointed resources it recovers are not compatible with the
// slave resources specified using the '--resources' flag.
TEST_F(ReservationTest, IncompatibleCheckpointedResources)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Wait for CheckpointResourcesMessage to be delivered.
  AWAIT_READY(checkpointResources);

  terminate(slave1);
  wait(slave1);

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it's not compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:4;mem:2048";

  MockSlave slave2(slaveFlags, &detector, &containerizer);

  Future<Future<Nothing>> recover;
  EXPECT_CALL(slave2, __recover(_))
    .WillOnce(DoAll(FutureArg<0>(&recover), Return()));

  spawn(slave2);

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect for 'recover' to have failed.
  AWAIT_FAILED(recover.get());

  terminate(slave2);
  wait(slave2);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that reserve and unreserve operations complete
// successfully when authorization succeeds.
TEST_F(ReservationTest, GoodACLReserveThenUnreserve)
{
  ACLs acls;

  // The principal of `DEFAULT_CREDENTIAL` can reserve resources for any role.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // The principal of `DEFAULT_CREDENTIAL` can unreserve
  // its own reserved resources.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  unreserve->mutable_reserver_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that a reserve operation
// gets dropped if authorization fails.
TEST_F(ReservationTest, BadACLDropReserve)
{
  ACLs acls;

  // No entity can reserve any resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, still expect an offer with unreserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that an unreserve operation
// gets dropped if authorization fails.
TEST_F(ReservationTest, BadACLDropUnreserve)
{
  ACLs acls;

  // This principal can reserve resources for any role.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This principal cannot unreserve any resources.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  unreserve->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Define the resources to be reserved.
  Resources unreserved1 = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 = unreserved1.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 = unreserved2.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  // The slave's total resources are twice those defined by `unreserved1`.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved1 + unreserved1, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the first set of resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved1)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // The reserved resources and an equal portion of
  // unreserved resources should be present.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(
          dynamicallyReserved1 + unreserved1,
          frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the first set of resources, and reserve the second set.
  driver.acceptOffers({offer.id()},
      {UNRESERVE(dynamicallyReserved1),
       RESERVE(dynamicallyReserved2)},
      filters);

  // In the next offer, expect to find both sets of reserved
  // resources, since the Unreserve operation should fail.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(
          dynamicallyReserved1 + dynamicallyReserved2 + unreserved2,
          frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// Tests a couple more complex combinations of `RESERVE`, `UNRESERVE`, and
// `LAUNCH` offer operations to verify that they work with authorization.
TEST_F(ReservationTest, ACLMultipleOperations)
{
  // Pause the clock and control it manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  ACLs acls;

  // This principal can reserve resources for any role.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // This principal cannot unreserve any resources.
  mesos::ACL::UnreserveResources* unreserve = acls.add_unreserve_resources();
  unreserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  unreserve->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Define the resources to be reserved.
  Resources unreserved1 = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 = unreserved1.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 = unreserved2.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // Advance the clock to generate an offer.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the first offer, expect an offer with unreserved resources.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  // The slave's total resources are twice those defined by `unreserved1`.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved1 + unreserved1, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the first set of resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved1)}, filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // The reserved resources and an equal portion of
  // unreserved resources should be present.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(
          dynamicallyReserved1 + unreserved1,
          frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Create a task to launch with the resources of `dynamicallyReserved2`.
  TaskInfo taskInfo1 =
    createTask(offer.slave_id(), dynamicallyReserved2, "exit 1", exec.id);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Expect a TASK_FINISHED status.
  Future<TaskStatus> statusUpdateAcknowledgement;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusUpdateAcknowledgement));

  // Attempt to unreserve a set of resources,
  // reserve a second set, and launch a task.
  driver.acceptOffers({offer.id()},
      {UNRESERVE(dynamicallyReserved1),
       RESERVE(dynamicallyReserved2),
       LAUNCH({taskInfo1})},
      filters);

  // Wait for TASK_FINISHED update ack.
  AWAIT_READY(statusUpdateAcknowledgement);
  EXPECT_EQ(TASK_FINISHED, statusUpdateAcknowledgement->state());

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect to find both sets of reserved
  // resources, since the Unreserve operation should fail.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(
          dynamicallyReserved1 + dynamicallyReserved2 + unreserved2,
          frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Create a task to launch with the resources of `dynamicallyReserved1`.
  TaskInfo taskInfo2 =
    createTask(offer.slave_id(), dynamicallyReserved1, "exit 1", exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> failedTaskStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&failedTaskStatus));

  // Attempt to unreserve all the dynamically reserved resources
  // and launch a task on `dynamicallyReserved1`.
  driver.acceptOffers({offer.id()},
      {UNRESERVE(dynamicallyReserved1),
       UNRESERVE(dynamicallyReserved2),
       LAUNCH({taskInfo2})},
      filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect to find the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(
          dynamicallyReserved1 + dynamicallyReserved2 + unreserved2,
          frameworkInfo.role())));

  // Check that the task launched as expected.
  EXPECT_EQ(TASK_FINISHED, failedTaskStatus->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Confirms that reserve and unreserve operations work without authentication
// when a framework has no principal.
TEST_F(ReservationTest, WithoutAuthenticationWithoutPrincipal)
{
  // Pause the clock and control it manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  // Create a framework without a principal.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.clear_principal();

  // Create a master with no framework authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo, master.get()->pid);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  // Create dynamically reserved resources whose `ReservationInfo` does not
  // contain a principal.
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo()).get();

  // We use this to capture offers from `resourceOffers`.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // An expectation for an offer with unreserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the offer with reserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Attempt to reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // Make sure that the reservation succeeded.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // An expectation for an offer with unreserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers(
      {offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// Confirms that reserve and unreserve operations work without authentication
// when a framework has a principal.
TEST_F(ReservationTest, WithoutAuthenticationWithPrincipal)
{
  // Pause the clock and control it manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  // Create a framework with a principal.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master with no framework authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo, master.get()->pid);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  // Create dynamically reserved resources whose `ReservationInfo` contains a
  // principal.
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from `resourceOffers`.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // An expectation for an offer with unreserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the offer with reserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Attempt to reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // Make sure that the reservation succeeded.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // An expectation for an offer with unreserved resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers(
      {offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This tests that a framework can't reserve resources using a role different
// from the one it registered with.
TEST_F(ReservationTest, DropReserveWithInvalidRole)
{
  const string frameworkRole = "role";
  const string invalidRole = "invalid-role";

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(frameworkRole);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:1;mem:512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers->front();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve resources using a different role than the one the
  // framework is registered with.
  Resources dynamicallyReservedInvalidRole = unreserved.flatten(
      invalidRole,
      createReservationInfo(frameworkInfo.principal())).get();

  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReservedInvalidRole)},
      filters);

  // In the next offer, still expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test ensures that a framework can't unreserve resources
// reserved by a framework with another role.
TEST_F(ReservationTest, PreventUnreservingAlienResources)
{
  const string frameworkRole1 = "role1";
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_role(frameworkRole1);

  const string frameworkRole2 = "role2";
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework2");
  frameworkInfo2.set_role(frameworkRole2);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:1;mem:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers->front();

  const Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo1.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve half the memory for `frameworkRole1`.
  const Resources halfMemory = Resources::parse("mem:256").get();
  const Resources dynamicallyReserved =
    halfMemory
      .flatten(
          frameworkRole1, createReservationInfo(frameworkInfo1.principal()))
      .get();

  driver1.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.role())));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo1.role())));

  // The filter to decline the offer "forever".
  Filters filtersForever;
  filtersForever.set_refuse_seconds(std::numeric_limits<double>::max());

  // Decline the offer "forever" in order to force `framework2` to
  // receive the remaining resources.
  driver1.declineOffer(offer.id(), filtersForever);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // The expectation for `driver2`'s first offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  // Expect an offer without the resources reserved by `framework1`.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo2.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo2.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the unreserve offer operation will be dropped and hence
  // allocator not called at all.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // Try to make `framework2` "steal" the resources reserved by `framework1`.
  driver2.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // Expect another offer without the resources reserved by `framework1`.
  AWAIT_READY(offers);

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo2.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo2.role())));

  // Decline the offer "forever" in order to force `framework1` to
  // receive the remaining resources.
  driver2.declineOffer(offer.id(), filtersForever);

  driver2.stop();
  driver2.join();

  // The expectation for the last offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Make the allocator ignore the filters.
  driver1.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  // Make sure that the reservation is still in place.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo1.role())));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.role())));

  driver1.stop();
  driver1.join();
}

}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
