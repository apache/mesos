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

class ReservationTest : public MesosTest
{
public:
  // Depending on the agent capability, the master will send different
  // messages to the agent when a reservation is applied.
  template <typename To>
  Future<Resources> getOperationMessage(To to)
  {
    return FUTURE_PROTOBUF(ApplyOperationMessage(), _, to)
      .then([](const ApplyOperationMessage& message) {
        switch (message.operation_info().type()) {
          case Offer::Operation::UNKNOWN:
          case Offer::Operation::LAUNCH:
          case Offer::Operation::LAUNCH_GROUP:
          case Offer::Operation::CREATE_DISK:
          case Offer::Operation::DESTROY_DISK:
          case Offer::Operation::GROW_VOLUME:
          case Offer::Operation::SHRINK_VOLUME:
            UNREACHABLE();
          case Offer::Operation::RESERVE: {
            Resources resources =
              message.operation_info().reserve().resources();
            resources.unallocate();

            return resources;
          }
          case Offer::Operation::UNRESERVE: {
            Resources resources =
              message.operation_info().unreserve().resources();
            resources.unallocate();

            return resources;
          }
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
};


// This tests that a framework can send back a Reserve operation
// as a response to an offer, which updates the resources in the
// allocator and results in the reserved resources being reoffered to
// the framework. The framework then sends back an Unreserved offer
// operation to unreserve the reserved resources. Finally, We test
// that the framework receives the unreserved resources.
TEST_F(ReservationTest, ReserveThenUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:24;mem:4096";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered (default would be 5 seconds).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:0.1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
    reserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(finalReservation, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that a framework can send back a Reserve followed by a
// LaunchTasks operation as a response to an offer, which
// updates the resources in the allocator then proceeds to launch the
// task with the reserved resources. The reserved resources are
// reoffered to the framework on task completion. The framework then
// sends back an Unreserved operation to unreserve the reserved
// resources. We test that the framework receives the unreserved
// resources.
TEST_F(ReservationTest, ReserveAndLaunchThenUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test launches 2 frameworks in the same role. framework1
// reserves resources by sending back a Reserve operation. We
// first test that framework1 receives the reserved resources, then on
// the next resource offer, framework1 declines the offer. This
// should lead to framework2 receiving the resources that framework1
// reserved.
TEST_F(ReservationTest, ReserveShareWithinRole)
{
  string role = "role";

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_roles(0, role);

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework2");
  frameworkInfo2.set_roles(0, role);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = role;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo(role, frameworkInfo1.principal()));

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
      allocatedResources(unreserved, frameworkInfo1.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo1.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo1.roles(0))));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This tests that a Reserve operation where the specified resources
// does not exist in the given offer (too large, in this case) is
// dropped.
TEST_F(ReservationTest, DropReserveTooLarge)
{
  TestAllocator<> allocator;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources unreservedTooLarge = Resources::parse("cpus:1;mem:1024").get();
  Resources dynamicallyReservedTooLarge =
    unreservedTooLarge.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  // We use this to capture the offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework_(_, _, _, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve operation will be dropped.
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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test verifies that the slave checkpoints the resources for
// dynamic reservations to the disk, recovers them upon restart, and
// sends them to the master during re-registration.
TEST_F(ReservationTest, ResourcesCheckpointing)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.recover = "reconnect";
  slaveFlags.resources = "cpus:8;mem:4096";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the operation message.
  AWAIT_READY(message);

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

  ReregisterSlaveMessage reregisterSlave_ = reregisterSlave.get();
  upgradeResources(&reregisterSlave_);

  EXPECT_EQ(reregisterSlave_.checkpointed_resources(), reserved);

  driver.stop();
  driver.join();
}


// This test verifies the case where a slave that has checkpointed
// dynamic reservations reregisters with a failed over master, and the
// dynamic reservations are later correctly offered to the framework.
TEST_F(ReservationTest, MasterFailover)
{
  // Pause the cock and control it manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master1 = StartMaster(masterFlags);
  ASSERT_SOME(master1);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:2048";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master1.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to generate an offer.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect to receive the operation message.
  AWAIT_READY(message);

  // This is to make sure operation message is processed.
  Clock::settle();

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate master failover by restarting the master.
  master1->reset();

  Try<Owned<cluster::Master>> master2 = StartMaster(masterFlags);
  ASSERT_SOME(master2);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  // Simulate a new master detected event on the slave so that the
  // slave will do a re-registration.
  detector.appoint(master2.get()->pid);

  // Ensure agent registration is processed.
  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  Clock::resume();

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

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

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  Future<Resources> message = getOperationMessage(_);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Wait until the operation message arrives.
  AWAIT_READY(message);

  slave1.get()->terminate();

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it is compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:12;mem:2048";

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

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect 'recover' to have completed successfully.
  AWAIT_READY(recover.get());

  slave2.get()->terminate();

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
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096;disk:2048";

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

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

  Resource unreservedDisk = Resources::parse("disk", "1024", "*").get();
  Resource reservedDisk = unreservedDisk;
  reservedDisk.add_reservations()->CopyFrom(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved + unreservedDisk, frameworkInfo.roles(0))));

  Future<Resources> message2 = getOperationMessage(_);
  Future<Resources> message1 = getOperationMessage(_);

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
  EXPECT_TRUE(message1->contains(reserved + reservedDisk));

  AWAIT_READY(message2);
  EXPECT_TRUE(message2->contains(volume));

  // Expect an offer containing the volume.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved + volume, frameworkInfo.roles(0))));

  Future<OfferID> rescindedOfferId;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescindedOfferId));

  slave1.get()->terminate();

  AWAIT_READY(rescindedOfferId);
  EXPECT_EQ(rescindedOfferId.get(), offer.id());

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it is compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:12;mem:2048;disk:1024";

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

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect that 'recover' will complete successfully.
  AWAIT_READY(recover.get());

  slave2.get()->terminate();

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
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

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

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  Future<Resources> message = getOperationMessage(_);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Wait for the operation message to be delivered.
  AWAIT_READY(message);

  slave1.get()->terminate();

  // Simulate a reboot of the slave machine by modifying the boot ID.
  ASSERT_SOME(os::write(slave::paths::getBootIdPath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  // Change the slave resources so that it's not compatible with the
  // checkpointed resources.
  slaveFlags.resources = "cpus:4;mem:2048";

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

  // Wait for 'recover' to finish.
  AWAIT_READY(recover);

  // Expect for 'recover' to have failed.
  AWAIT_FAILED(recover.get());

  slave2.get()->terminate();

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
  frameworkInfo.set_roles(0, "role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  // Create a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

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
  Resources dynamicallyReserved1 =
    unreserved1.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 =
    unreserved2.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved1 + unreserved1, frameworkInfo.roles(0))));

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
          frameworkInfo.roles(0))));

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
          frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// Tests a couple more complex combinations of `RESERVE`, `UNRESERVE`, and
// `LAUNCH` operations to verify that they work with authorization.
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
  frameworkInfo.set_roles(0, "role");

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Create a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

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
  Resources dynamicallyReserved1 =
    unreserved1.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Resources unreserved2 = Resources::parse("cpus:0.5;mem:256").get();
  Resources dynamicallyReserved2 =
    unreserved2.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved1 + unreserved1, frameworkInfo.roles(0))));

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
          frameworkInfo.roles(0))));

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
          frameworkInfo.roles(0))));

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
          frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");
  frameworkInfo.clear_principal();

  // Create a master with no framework authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo, master.get()->pid);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  // Create dynamically reserved resources whose `ReservationInfo` does not
  // contain a principal.
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo(frameworkInfo.roles(0)));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

  // Create a master with no framework authentication.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo, master.get()->pid);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();

  // Create dynamically reserved resources whose `ReservationInfo` contains a
  // principal.
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This tests that a framework can't reserve resources using a role different
// from the one it registered with.
TEST_F(ReservationTest, DropReserveWithDifferentRole)
{
  const string frameworkRole = "role";

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, frameworkRole);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework_(_, _, _, _, _));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers->front();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Expect that the reserve operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve resources using a different role than the one the
  // framework is registered with.
  Resources dynamicallyReservedDifferentRole = unreserved.pushReservation(
      createDynamicReservationInfo("foo", frameworkInfo.principal()));

  driver.acceptOffers(
      {offer.id()},
      {RESERVE(dynamicallyReservedDifferentRole)},
      filters);

  // In the next offer, still expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo1.set_roles(0, frameworkRole1);

  const string frameworkRole2 = "role2";
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_roles(0, frameworkRole2);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:1;mem:512";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(updateSlaveMessage);

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
      allocatedResources(unreserved, frameworkInfo1.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve half the memory for `frameworkRole1`.
  const Resources halfMemory = Resources::parse("mem:256").get();
  const Resources dynamicallyReserved = halfMemory.pushReservation(
      createDynamicReservationInfo(frameworkRole1, frameworkInfo1.principal()));

  driver1.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.roles(0))));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo1.roles(0))));

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
      allocatedResources(halfMemory, frameworkInfo2.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo2.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the unreserve operation will be dropped and hence
  // allocator not called at all.
  EXPECT_CALL(allocator, updateAllocation(_, _, _, _))
    .Times(0);

  // Try to make `framework2` "steal" the resources reserved by `framework1`.
  driver2.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // Expect another offer without the resources reserved by `framework1`.
  AWAIT_READY(offers);

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(halfMemory, frameworkInfo2.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo2.roles(0))));

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
      allocatedResources(halfMemory, frameworkInfo1.roles(0))));
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo1.roles(0))));

  driver1.stop();
  driver1.join();
}


class ReservationCheckpointingTest : public MesosTest {};


// This test verifies that CheckpointResourcesMessages are sent to the
// slave when a framework reserve/unreserves resources, and the
// resources in the messages correctly reflect the resources that need
// to be checkpointed on the slave.
TEST_F(ReservationCheckpointingTest, SendingCheckpointResourcesMessage)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:4096";

  // The master only sends `CheckpointResourcesMessage` to
  // agents which are not resource provider-capable.
  slaveFlags.agent_features = SlaveCapabilities();

  foreach (
      const SlaveInfo::Capability& slaveCapability,
      slave::AGENT_CAPABILITIES()) {
    if (slaveCapability.type() != SlaveInfo::Capability::RESOURCE_PROVIDER) {
      slaveFlags.agent_features->add_capabilities()->CopyFrom(slaveCapability);
    }
  }

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved1 = Resources::parse("cpus:8").get();
  Resources reserved1 =
    unreserved1.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Resources unreserved2 = Resources::parse("mem:2048").get();
  Resources reserved2 =
    unreserved2.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved1 + unreserved2, frameworkInfo.roles(0))));

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


// This test verifies that the agent still checkpoints and recovers resources
// in the format used by agents that don't atomically checkpoint operations and
// resources.
//
// To verify this the test does the following:
//
// 1. Start a master, agent, and a framework.
// 2. Make the framework reserve resources and assert that they are offered
//    back.
// 3. Kill the agent.
// 4. Remove the file that contains the operations and resources.
// 5. Start a new agent using the same metadata directory.
// 6. Start a new framework and assert that it is offered the resources that
//    were reserved in step #2.
TEST_F(ReservationTest, ReservationCheckpointedBackwardsCompatibility)
{
  // Pause the cock and control it manually in order to
  // control the timing of the offer cycle.
  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;mem:2048";

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Resources unreserved = Resources::parse("cpus:8;mem:2048").get();
  Resources reserved = unreserved.pushReservation(createDynamicReservationInfo(
      frameworkInfo.roles(0), frameworkInfo.principal()));

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  // Set an expectation for the first offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  // Advance the clock to generate an offer.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  Future<Resources> message = getOperationMessage(slave.get()->pid);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver1.acceptOffers({offer.id()}, {RESERVE(reserved)}, filters);

  // Expect the agent to receive the operation message.
  AWAIT_READY(message);

  // This is to make sure the agent processes the operation message.
  Clock::settle();

  // Set an expectation for the next offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  // Advance the clock to generate an offer.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved, frameworkInfo.roles(0))));

  // Stop the framework.
  driver1.stop();
  driver1.join();

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, _);

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Simulate agent failover.
  slave.get()->terminate();
  slave->reset();

  string resourceStatePath = slave::paths::getResourceStatePath(
      slave::paths::getMetaRootDir(slaveFlags.work_dir));
  CHECK_SOME(os::rm(resourceStatePath));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock, so that the agent re-registers.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Resume the clock to avoid deadlocks related to agent registration.
  // See MESOS-8828.
  Clock::resume();

  // Wait for the agent to re-register.
  AWAIT_READY(slaveReregistered);
  AWAIT_READY(updateSlaveMessage);

  Clock::pause();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Set an expectation for the next new framework's first offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  driver2.start();

  // Advance the clock to generate an offer.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(reserved, frameworkInfo.roles(0))));

  driver2.stop();
  driver2.join();
}

}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
