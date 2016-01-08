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

#include <mesos/master/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <stout/some.hpp>
#include <stout/strings.hpp>

#include "master/constants.hpp"
#include "master/detector.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Try<PID<Slave>> slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      role, createReservationInfo(frameworkInfo1.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources unreservedTooLarge = Resources::parse("cpus:1;mem:1024").get();
  Resources dynamicallyReservedTooLarge = unreservedTooLarge.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture the offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _))
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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus(role):1;mem(role):512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _));

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources staticallyReserved =
    Resources::parse("cpus(role):1;mem(role):512").get();
  Resources dynamicallyReserved = staticallyReserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _));

  driver.start();

  // In the first offer, expect an offer with the statically reserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(staticallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _))
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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(staticallyReserved));

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved1 = Resources::parse("cpus:8").get();
  Resources reserved1 = unreserved1.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  Resources unreserved2 = Resources::parse("mem:2048").get();
  Resources reserved2 = unreserved2.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved1 + unreserved2));

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
  EXPECT_EQ(Resources(message1.get().resources()), reserved1);

  // Expect the 'RESERVE(reserved2)' as the second message.
  // The checkpointed resources should correspond to
  // 'reserved1 + reserved2'.
  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2.get().resources()), reserved1 + reserved2);

  // Expect the 'UNRESERVE(reserved1)' as the third message.
  // The checkpointed resources should correspond to 'reserved2'.
  AWAIT_READY(message3);
  EXPECT_EQ(Resources(message3.get().resources()), reserved2);

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.recover = "reconnect";
  slaveFlags.resources = "cpus:8;mem:4096";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the 'CheckpointResourcesMessage'.
  AWAIT_READY(checkpointResources);

  // Restart the slave without shutting down.
  Stop(slave.get(), false);

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  Future<Nothing> slaveRecover = FUTURE_DISPATCH(_, &Slave::recover);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Expect the slave to recover.
  AWAIT_READY(slaveRecover);

  // Expect to receive the 'ReregisterSlaveMessage' containing the
  // reserved resources.
  AWAIT_READY(reregisterSlave);

  EXPECT_EQ(reregisterSlave.get().checkpointed_resources(), reserved);

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master1 = StartMaster(masterFlags);
  ASSERT_SOME(master1);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:2048";

  StandaloneMasterDetector detector(master1.get());

  Try<PID<Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  Future<CheckpointResourcesMessage> checkpointResources =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, slave.get());

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the CheckpointResourcesMessage.
  AWAIT_READY(checkpointResources);

  // This is to make sure CheckpointResourcesMessage is processed.
  process::Clock::pause();
  process::Clock::settle();
  process::Clock::resume();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate master failover by restarting the master.
  Stop(master1.get());

  Try<PID<Master>> master2 = StartMaster(masterFlags);
  ASSERT_SOME(master2);

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master2.get());

  // Wait for slave to confirm re-registration.
  AWAIT_READY(slaveReregistered);

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(reserved));

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get());

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

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

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

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

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096;disk:2048";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get());

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  Resource unreservedDisk = Resources::parse("disk", "1024", "*").get();
  Resource reservedDisk = unreservedDisk;
  reservedDisk.set_role(frameworkInfo.role());
  reservedDisk.mutable_reservation()->CopyFrom(
      createReservationInfo(frameworkInfo.principal()));

  Resource volume = reservedDisk;
  volume.mutable_disk()->CopyFrom(
      createDiskInfo("persistence_id", "container_path"));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(
      Resources(offer.resources()).contains(unreserved + unreservedDisk));

  Future<CheckpointResourcesMessage> message2 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  Future<CheckpointResourcesMessage> message1 =
    FUTURE_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources and create the volume.
  driver.acceptOffers(
      {offer.id()},
      {RESERVE(reserved + reservedDisk), CREATE(volume)},
      filters);

  // NOTE: Currently, we send one message per operation. But this is
  // an implementation detail which is subject to change.
  AWAIT_READY(message1);
  EXPECT_EQ(Resources(message1.get().resources()), reserved + reservedDisk);

  AWAIT_READY(message2);
  EXPECT_EQ(Resources(message2.get().resources()), reserved + volume);

  terminate(slave1);
  wait(slave1);

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

  driver.stop();
  driver.join();

  Shutdown();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get());

  MockSlave slave1(slaveFlags, &detector, &containerizer);
  spawn(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

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

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that reserve and unreserve operations complete
// successfully when authorization succeeds.
TEST_F(ReservationTest, GoodACLReserveThenUnreserve)
{
  ACLs acls;

  // This principal can reserve any resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::ANY);

  // This principal can unreserve any resources.
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that a reserve operation
// gets dropped if authorization fails.
TEST_F(ReservationTest, BadACLDropReserve)
{
  ACLs acls;

  // No entity can reserve any resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::ANY);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.role();

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, still expect an offer with unreserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that an unreserve operation
// gets dropped if authorization fails.
TEST_F(ReservationTest, BadACLDropUnreserve)
{
  ACLs acls;

  // This principal can reserve any resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::ANY);

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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Define the resources to be reserved.
  Resources unreserved1 = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 = unreserved1.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));
  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 = unreserved2.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  // The slave's total resources are twice those defined by `unreserved1`.
  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved1 + unreserved1));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the first set of resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved1)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  // The reserved resources and an equal portion of
  // unreserved resources should be present.
  EXPECT_TRUE(
      Resources(offer.resources()).contains(
          dynamicallyReserved1 +
          unreserved1));

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

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
          dynamicallyReserved1 +
          dynamicallyReserved2 +
          unreserved2));

  driver.stop();
  driver.join();

  Shutdown();
}


// Tests a couple more complex combinations of `RESERVE`, `UNRESERVE`, and
// `LAUNCH` offer operations to verify that they work with authorization.
TEST_F(ReservationTest, ACLMultipleOperations)
{
  // Pause the clock and control it manually in order to
  // control the timing of the offer cycle.
  process::Clock::pause();

  ACLs acls;

  // This principal can reserve any resources.
  mesos::ACL::ReserveResources* reserve = acls.add_reserve_resources();
  reserve->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  reserve->mutable_resources()->set_type(mesos::ACL::Entity::ANY);

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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Try<PID<Slave>> slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Define the resources to be reserved.
  Resources unreserved1 = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved1 = unreserved1.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));
  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 = unreserved2.flatten(
      frameworkInfo.role(), createReservationInfo(frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // Advance the clock to generate an offer.
  process::Clock::settle();
  process::Clock::advance(masterFlags.allocation_interval);

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  Offer offer = offers.get()[0];

  // The slave's total resources are twice those defined by `unreserved1`.
  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved1 + unreserved1));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the first set of resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved1)}, filters);

  process::Clock::settle();
  process::Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  // The reserved resources and an equal portion of
  // unreserved resources should be present.
  EXPECT_TRUE(
      Resources(offer.resources()).contains(
          dynamicallyReserved1 +
          unreserved1));

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
  EXPECT_EQ(TASK_FINISHED, statusUpdateAcknowledgement.get().state());

  process::Clock::settle();
  process::Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect to find both sets of reserved
  // resources, since the Unreserve operation should fail.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
          dynamicallyReserved1 +
          dynamicallyReserved2 +
          unreserved2));

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

  process::Clock::settle();
  process::Clock::advance(masterFlags.allocation_interval);

  // In the next offer, expect to find the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());
  offer = offers.get()[0];

  EXPECT_TRUE(
      Resources(offer.resources()).contains(
          dynamicallyReserved1 +
          dynamicallyReserved2 +
          unreserved2));

  // Check that the task launched as expected.
  EXPECT_EQ(TASK_FINISHED, failedTaskStatus.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}
}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
