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

#include <gmock/gmock.h>

#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

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

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;
using process::Future;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::SaveArg;

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
  masterFlags.allocation_interval = Milliseconds(50);
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

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (by default).
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

  // In the first offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that a framework can send back a Reserve followed by
// a LaunchTasks offer operation as a response to an offer, which
// updates the resources in the allocator then proceeds to launch
// the task with the reserved resources. The reserved resources are
// reoffered to the framework on task completion. The framework then
// sends back an Unreserved offer operation to unreserve the reserved
// resources. We test that the framework receives the unreserved
// resources.
TEST_F(ReservationTest, ReserveAndLaunchThenUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
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

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
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

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {UNRESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test launches 2 frameworks in the same role. framework1
// reserves resources by sending back a Reserve offer operation.
// We first test that framework1 receives the reserved resources,
// then on the next resource offer, framework1 declines the offer.
// This should lead to framework2 receiving the resources that
// framework1 reserved.
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
  masterFlags.allocation_interval = Milliseconds(50);
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

  // The expectation for the first offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  driver1.start();

  // In the first offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Reserve the resources.
  driver1.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with the reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  // The filter to decline the offer "forever".
  Filters filtersForever;
  filters.set_refuse_seconds(std::numeric_limits<double>::max());

  // Decline the offer "forever" in order to force framework2 to
  // receive the resources.
  driver1.declineOffer(offer.id(), filtersForever);

  // The expectation for the next offer.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  driver2.start();

  // In the next offer, expect an offer with the resources reserved by
  // framework1.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
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
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _));

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

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with the unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (by default).
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

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  driver.stop();
  driver.join();

  Shutdown();
}


// This tests that an attempt to dynamically reserve statically reserved
// resources is dropped.
TEST_F(ReservationTest, DropReserveStaticReservation)
{
  TestAllocator<> allocator;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  masterFlags.roles = frameworkInfo.role();

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<PID<Master>> master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus(role):1;mem(role):512";

  EXPECT_CALL(allocator, addSlave(_, _, _, _));

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

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(allocator, addFramework(_, _, _));

  EXPECT_CALL(sched, registered(&driver, _, _));

  driver.start();

  // In the first offer, expect an offer with the statically
  // reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(staticallyReserved));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect that the reserve offer operation will be dropped.
  EXPECT_CALL(allocator, updateAllocation(_, _, _))
    .Times(0);

  // We use the filter explicitly here so that the resources
  // will not be filtered for 5 seconds (by default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Attempt to reserve the statically reserved resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, still expect an offer with the statically reserved
  // resources.
  AWAIT_READY(offers);

  ASSERT_EQ(offers.get().size(), 1);
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(staticallyReserved));

  driver.stop();
  driver.join();

  Shutdown();
}

}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
