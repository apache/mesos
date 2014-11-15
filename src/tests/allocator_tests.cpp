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

#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <stout/some.hpp>

#include "master/allocator.hpp"
#include "master/detector.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::allocator::Allocator;
using mesos::internal::master::allocator::AllocatorProcess;
using mesos::internal::master::allocator::HierarchicalDRFAllocatorProcess;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::SaveArg;


class DRFAllocatorTest : public MesosTest {};


// Checks that the DRF allocator implements the DRF algorithm
// correctly. The test accomplishes this by adding frameworks and
// slaves one at a time to the allocator, making sure that each time
// a new slave is added all of its resources are offered to whichever
// framework currently has the smallest share. Checking for proper DRF
// logic when resources are returned, frameworks exit, etc. is handled
// by SorterTest.DRFSorter.
TEST_F(DRFAllocatorTest, DRFAllocatorProcess)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = Some("role1,role2");
  Try<PID<Master> > master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  slave::Flags flags1 = CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = StartSlave(flags1);
  ASSERT_SOME(slave1);
  // Total cluster resources now cpus=2, mem=1024.

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_role("role1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(offers1);

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far.
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));
  // user1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 1

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_role("role2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> frameworkAdded2;
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
      FutureSatisfy(&frameworkAdded2)));

  EXPECT_CALL(sched2, registered(_, _, _));

  driver2.start();

  AWAIT_READY(frameworkAdded2);

  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Some("cpus:1;mem:512;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  Try<PID<Slave> > slave2 = StartSlave(flags2);
  ASSERT_SOME(slave2);
  // Total cluster resources now cpus=3, mem=1536.
  // user1 share = 0.66 (cpus=2, mem=1024)
  //   framework1 share = 1
  // user2 share = 0
  //   framework2 share = 0

  AWAIT_READY(offers2);

  // framework2 will be offered all of slave2's resources since user2
  // has the lowest user share, and framework2 is its only framework.
  EXPECT_THAT(offers2.get(), OfferEq(1, 512));
  // user1 share = 0.67 (cpus=2, mem=1024)
  //   framework1 share = 1
  // user2 share = 0.33 (cpus=1, mem=512)
  //   framework2 share = 1

  slave::Flags flags3 = CreateSlaveFlags();
  flags3.resources = Some("cpus:3;mem:2048;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3));

  Try<PID<Slave> > slave3 = StartSlave(flags3);
  ASSERT_SOME(slave3);
  // Total cluster resources now cpus=6, mem=3584.
  // user1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // user2 share = 0.16 (cpus=1, mem=512)
  //   framework2 share = 1

  AWAIT_READY(offers3);

  // framework2 will be offered all of slave3's resources since user2
  // has the lowest share.
  EXPECT_THAT(offers3.get(), OfferEq(3, 2048));
  // user1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // user2 share = 0.71 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo frameworkInfo3; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo3 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_user("user3");
  frameworkInfo3.set_role("role1");

  MockScheduler sched3;
  MesosSchedulerDriver driver3(
      &sched3, frameworkInfo3, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> frameworkAdded3;
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
      FutureSatisfy(&frameworkAdded3)));

  EXPECT_CALL(sched3, registered(_, _, _));

  driver3.start();

  AWAIT_READY(frameworkAdded3);

  slave::Flags flags4 = CreateSlaveFlags();
  flags4.resources = Some("cpus:4;mem:4096;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers4;
  EXPECT_CALL(sched3, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers4));

  Try<PID<Slave> > slave4 = StartSlave(flags4);
  ASSERT_SOME(slave4);
  // Total cluster resources now cpus=10, mem=7680.
  // user1 share = 0.2 (cpus=2, mem=1024)
  //   framework1 share = 1
  //   framework3 share = 0
  // user2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1

  AWAIT_READY(offers4);

  // framework3 will be offered all of slave4's resources since user1
  // has the lowest user share, and framework3 has the lowest share of
  // user1's frameworks.
  EXPECT_THAT(offers4.get(), OfferEq(4, 4096));
  // user1 share = 0.67 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  // user2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo frameworkInfo4; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo4 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo4.set_name("framework4");
  frameworkInfo4.set_user("user1");
  frameworkInfo4.set_role("role1");
  MockScheduler sched4;
  MesosSchedulerDriver driver4(
      &sched4, frameworkInfo4, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> frameworkAdded4;
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
                    FutureSatisfy(&frameworkAdded4)));

  EXPECT_CALL(sched4, registered(_, _, _));

  driver4.start();

  AWAIT_READY(frameworkAdded4);

  slave::Flags flags5 = CreateSlaveFlags();
  flags5.resources = Some("cpus:1;mem:512;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers5;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers5));

  Try<PID<Slave> > slave5 = StartSlave(flags5);
  ASSERT_SOME(slave5);
  // Total cluster resources now cpus=11, mem=8192
  // user1 share = 0.63 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  //   framework4 share = 0
  // user2 share = 0.36 (cpus=4, mem=2560)
  //   framework2 share = 1
  AWAIT_READY(offers5);

  // Even though framework4 doesn't have any resources, user2 has a
  // lower share than user1, so framework2 receives slave4's resources.
  EXPECT_THAT(offers5.get(), OfferEq(1, 512));

  // Shut everything down.
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .Times(AtMost(4));

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .Times(AtMost(4));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  driver3.stop();
  driver3.join();

  driver4.stop();
  driver4.join();

  EXPECT_CALL(allocator, slaveRemoved(_))
    .Times(AtMost(5));

  Shutdown();
}


// This test ensures that allocation is done per slave. This is done
// by having 2 slaves and 2 frameworks and making sure each framework
// gets only one slave's resources during an allocation.
TEST_F(DRFAllocatorTest, PerSlaveAllocation)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  // Start the master.
  // NOTE: We set a high allocation interval, so that allocator does
  // allocations only based on events (framework added, slave added)
  // but not due to allocation interval. This lets us tightly control
  // the test expectations.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = Some("role1,role2");
  masterFlags.allocation_interval = Days(1);
  Try<PID<Master> > master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Start slave 1.
  slave::Flags flags1 = CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024;disk:0");

  Future<Nothing> slaveAdded1;
  EXPECT_CALL(allocator, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&allocator),
                    FutureSatisfy(&slaveAdded1)));

  Try<PID<Slave> > slave1 = StartSlave(flags1);
  ASSERT_SOME(slave1);

  AWAIT_READY(slaveAdded1);

  // Start slave 2.
  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Some("cpus:2;mem:1024;disk:0");

  Future<Nothing> slaveAdded2;
  EXPECT_CALL(allocator, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&allocator),
                    FutureSatisfy(&slaveAdded2)));

  Try<PID<Slave> > slave2 = StartSlave(flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(slaveAdded2);

  // Start framework 1.
  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_role("role1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<Nothing> resourcesRecovered1;
  Future<Nothing> resourcesRecovered2;
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesRecovered(&allocator),
                    FutureSatisfy(&resourcesRecovered1)))
    .WillOnce(DoAll(InvokeResourcesRecovered(&allocator),
                    FutureSatisfy(&resourcesRecovered2)));

  // Decline the offers immediately so that resources for both slaves
  // are eligible for allocation to this and other frameworks.
  Filters filters;
  filters.set_refuse_seconds(0);
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(DeclineOffers(filters));

  driver1.start();

  // Wait until the resources are returned to the allocator.
  // NOTE: No allocations will be made after this point until a new
  // framework registers because
  // 1) 'resourcesRecovered' does not trigger an allocation and
  // 2) 'flags.allocation_interval' is set to a very high value.
  AWAIT_READY(resourcesRecovered1);
  AWAIT_READY(resourcesRecovered2);

  // Start framework 2.
  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_role("role2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // Offers to framework 1.
  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Offers to framework 2.
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  driver2.start();

  // Now each framework should receive offers for one slave each.
  AWAIT_READY(offers1);
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));

  AWAIT_READY(offers2);
  EXPECT_THAT(offers2.get(), OfferEq(2, 1024));

  // Shut everything down.
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .WillRepeatedly(DoDefault());

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  EXPECT_CALL(allocator, slaveRemoved(_))
    .WillRepeatedly(DoDefault());

  Shutdown();
}


// Helper that simply increments the value by reference.
ACTION_P(Increment, value) { *value += 1; }


// This test ensures that frameworks that have the same share get an
// equal number of allocations over time (rather than the same
// framework getting all the allocations because it's name is
// lexicographically ordered first).
TEST_F(DRFAllocatorTest, SameShareAllocations)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Start the first scheduler.
  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  Future<Nothing> registered1;
  EXPECT_CALL(sched1, registered(_, _, _))
    .WillOnce(FutureSatisfy(&registered1));

  driver1.start();

  AWAIT_READY(registered1);

  // Start the second scheduler.
  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  // We need to retire this expectation on the first match because
  // framework1 can match this expectation first in which case
  // framework2 should be able to match the expectation above.
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .RetiresOnSaturation();

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  driver2.start();

  AWAIT_READY(registered2);

  // Set filter timeout to 0 so that both frameworks are eligible
  // for allocation during every allocation interval.
  Filters filters;
  filters.set_refuse_seconds(0);

  int allocations1 = 0;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DoAll(Increment(&allocations1),
                          DeclineOffers(filters)));

  int allocations2 = 0;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DoAll(Increment(&allocations2),
                          DeclineOffers(filters)));

  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  // Start the slave.
  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  // Continuously do allocations.
  Clock::pause();
  while(allocations1 + allocations2 < 10) {
    Clock::advance(masterFlags.allocation_interval);
    Clock::settle();
  }

  // Each framework should get equal number of allocations.
  ASSERT_EQ(allocations1, allocations2);

  Clock::resume();

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
}


class ReservationAllocatorTest : public MesosTest {};


// Checks that resources on a slave that are statically reserved to
// a role are only offered to frameworks in that role.
TEST_F(ReservationAllocatorTest, ReservedResources)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = Some("role1,role2,role3");
  Try<PID<Master> > master = StartMaster(&allocator, masterFlags);

  ASSERT_SOME(master);

  Future<Nothing> slaveAdded;
  EXPECT_CALL(allocator, slaveAdded(_, _, _))
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoAll(InvokeSlaveAdded(&allocator),
                    FutureSatisfy(&slaveAdded)));

  slave::Flags flags1 = CreateSlaveFlags();
  flags1.default_role = "role1";
  flags1.resources = Some("cpus:2;mem:1024;disk:0");
  Try<PID<Slave> > slave1 = StartSlave(flags1);
  ASSERT_SOME(slave1);

  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources =
    Some("cpus(role2):2;mem(role2):1024;cpus:1;mem:1024;disk:0");
  Try<PID<Slave> > slave2 = StartSlave(flags2);
  ASSERT_SOME(slave2);

  slave::Flags flags3 = CreateSlaveFlags();
  flags3.default_role = "role3";
  flags3.resources = Some("cpus:4;mem:4096;disk:0");
  Try<PID<Slave> > slave3 = StartSlave(flags3);
  ASSERT_SOME(slave3);

  // This slave's resources should never be allocated,
  // since there is no framework for role4.
  slave::Flags flags4 = CreateSlaveFlags();
  flags4.default_role = "role4";
  flags4.resources = Some("cpus:1;mem:1024;disk:0");
  Try<PID<Slave> > slave4 = StartSlave(flags4);
  ASSERT_SOME(slave4);

  AWAIT_READY(slaveAdded);

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_role("role1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 2048)))
    .WillOnce(FutureSatisfy(&resourceOffers1));

  driver1.start();

  // framework1 gets all the resources from slave1, plus the
  // unreserved resources on slave2.
  AWAIT_READY(resourceOffers1);

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_role("role2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  driver2.start();

  // framework2 gets all of its reserved resources on slave2.
  AWAIT_READY(resourceOffers2);

  FrameworkInfo frameworkInfo3; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo3 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo3.set_user("user2");
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_role("role3");
  MockScheduler sched3;
  MesosSchedulerDriver driver3(
      &sched3, frameworkInfo3, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched3, registered(_, _, _));

  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched3, resourceOffers(_, OfferEq(4, 4096)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  driver3.start();

  // framework3 gets all the resources from slave3.
  AWAIT_READY(resourceOffers3);

  slave::Flags flags5 = CreateSlaveFlags();
  flags5.default_role = "role1";
  flags5.resources = Some("cpus:1;mem:512;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<Nothing> resourceOffers4;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(FutureSatisfy(&resourceOffers4));

  Try<PID<Slave> > slave5 = StartSlave(flags5);
  ASSERT_SOME(slave5);

  // framework1 gets all the resources from slave5.
  AWAIT_READY(resourceOffers4);

  // Shut everything down.
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .Times(AtMost(3));

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .Times(AtMost(3));

  driver3.stop();
  driver2.stop();
  driver1.stop();

  EXPECT_CALL(allocator, slaveRemoved(_))
    .Times(AtMost(5));

  this->Shutdown();
}


// Checks that statically allocated resources that are returned
// either unused or after a task finishes are statically reallocated
// appropriately.
TEST_F(ReservationAllocatorTest, ResourcesReturned)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = Some("role1,role2");
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = StartMaster(&allocator, masterFlags);

  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(allocator, slaveAdded(_, _, _))
    .Times(2);

  Future<Nothing> slaveAdded1 = FUTURE_DISPATCH(
      allocator.real, &AllocatorProcess::slaveAdded);

  slave::Flags flags1 = CreateSlaveFlags();
  flags1.resources = Some("cpus(role1):1;mem(role1):200;cpus(role2):2;"
                          "mem(role2):600;cpus:1;mem:200;disk:0");
  Try<PID<Slave> > slave1 = StartSlave(&exec, flags1);
  ASSERT_SOME(slave1);

  // Wait until allocator has added slave1.
  AWAIT_READY(slaveAdded1);

  Future<Nothing> slaveAdded2 = FUTURE_DISPATCH(
      allocator.real, &AllocatorProcess::slaveAdded);

  // This slave's resources will never be offered to anyone,
  // because there is no framework with role3.
  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Some("cpus(role3):4;mem(role3):1024;disk:0");
  Try<PID<Slave> > slave2 = StartSlave(flags2);
  ASSERT_SOME(slave2);

  // Wait until allocator has added slave2.
  AWAIT_READY(slaveAdded2);

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_role("role1");
  FrameworkID frameworkId1;

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  // Initially, framework1 should be offered all of the resources on
  // slave1 that aren't reserved to role2.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 400)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 100, "role1"));

  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(InvokeResourcesRecoveredWithFilters(&allocator, 0));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  // After framework1's task launches, it should be offered all resources
  // not dedicatd to role2 and not used by its task.
  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 300)))
    .WillOnce(FutureSatisfy(&resourceOffers1));

  driver1.start();

  AWAIT_READY(launchTask);

  AWAIT_READY(resourceOffers1);

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_role("role2");
  FrameworkID frameworkId2;

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // The first time framework2 is allocated to, it should be offered
  // all of the resources on slave1 that are reserved to role2.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 600)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  driver2.start();

  AWAIT_READY(resourceOffers2);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _));

  // After the task finishes, its resources should be reoffered to
  // framework1.
  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 100)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers3);

  // Shut everything down.
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .Times(AtMost(2));

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .Times(AtMost(2));

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver2.stop();
  driver1.stop();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.

  EXPECT_CALL(allocator, slaveRemoved(_))
    .Times(AtMost(2));

  this->Shutdown();
}


template <typename T>
class AllocatorTest : public MesosTest
{
protected:
  void StopAllocator()
  {
    process::terminate(allocator.real);
    process::wait(allocator.real);
  }

  MockAllocatorProcess<T> allocator;
};


// Causes all TYPED_TEST(AllocatorTest, ...) to be run for
// each of the specified Allocator classes.
TYPED_TEST_CASE(AllocatorTest, AllocatorTypes);


// Checks that in a cluster with one slave and one framework, all of
// the slave's resources are offered to the framework.
TYPED_TEST(AllocatorTest, MockAllocator)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:1024;disk:0");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // The framework should be offered all of the resources on the slave
  // since it is the only framework in the cluster.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that when a task is launched with fewer resources than what
// the offer was for, the resources that are returned unused are
// reoffered appropriately.
TYPED_TEST(AllocatorTest, ResourcesUnused)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->StartSlave(&exec, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first offer will contain all of the slave's resources, since
  // this is the only framework running so far. Launch a task that
  // uses less than that to leave some resources unused.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"));

  Future<Nothing> resourcesRecovered;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesRecovered(&this->allocator),
                    FutureSatisfy(&resourcesRecovered)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  AWAIT_READY(launchTask);

  // We need to wait until the allocator knows about the unused
  // resources to start the second framework so that we get the
  // expected offer.
  AWAIT_READY(resourcesRecovered);

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // We should expect that framework2 gets offered all of the
  // resources on the slave not being used by the launched task.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(2));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(2));

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Tests the situation where a frameworkRemoved call is dispatched
// while we're doing an allocation to that framework, so that
// resourcesRecovered is called for an already removed framework.
TYPED_TEST(AllocatorTest, OutOfOrderDispatch)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->StartSlave(flags1);
  ASSERT_SOME(slave1);

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(InvokeFrameworkAdded(&this->allocator));

  FrameworkID frameworkId1;
  EXPECT_CALL(sched1, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId1));

  // All of the slave's resources should be offered to start.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver1.start();

  AWAIT_READY(resourceOffers);

  // TODO(benh): I don't see why we want to "catch" (i.e., block) this
  // resourcesRecovered call. It seems like we want this one to
  // properly be executed and later we want to _inject_ a
  // resourcesRecovered to simulate the code in Master::offer after a
  // framework has terminated or is inactive.
  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources savedResources;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    // "Catches" the resourcesRecovered call from the master, so
    // that it doesn't get processed until we redispatch it after
    // the frameworkRemoved trigger.
    .WillOnce(DoAll(SaveArg<0>(&frameworkId),
                    SaveArg<1>(&slaveId),
                    SaveArg<2>(&savedResources)));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(frameworkId1)))
    .WillOnce(DoAll(InvokeFrameworkRemoved(&this->allocator),
                    FutureSatisfy(&frameworkRemoved)));

  driver1.stop();
  driver1.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoDefault());

  // Re-dispatch the resourcesRecovered call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  this->allocator.resourcesRecovered(
      frameworkId,
      slaveId,
      savedResources,
      None());

  // TODO(benh): Seems like we should wait for the above
  // resourcesRecovered to be executed.

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(InvokeFrameworkAdded(&this->allocator));

  FrameworkID frameworkId2;
  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId2));

  // All of the slave's resources should be offered since no other
  // frameworks should be running.
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(frameworkId2)))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that if a framework launches a task and then fails over to a
// new scheduler, the task's resources are not reoffered as long as it
// is running.
TYPED_TEST(AllocatorTest, SchedulerFailover)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_failover_timeout(10);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers()); // For subsequent offers.

  // Initially, all of slave1's resources are avaliable.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 256, "*"));

  // We don't filter the unused resources to make sure that
  // they get offered to the framework as soon as it fails over.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(InvokeResourcesRecoveredWithFilters(&this->allocator, 0))
    // For subsequent offers.
    .WillRepeatedly(InvokeResourcesRecoveredWithFilters(&this->allocator, 0));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  // Ensures that the task has been completely launched
  // before we have the framework fail over.
  AWAIT_READY(launchTask);

  // When we shut down the first framework, we don't want it to tell
  // the master it's shutting down so that the master will wait to see
  // if it fails over.
  DROP_PROTOBUFS(UnregisterFrameworkMessage(), _, _);

  Future<Nothing> frameworkDeactivated;
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillOnce(DoAll(InvokeFrameworkDeactivated(&this->allocator),
                    FutureSatisfy(&frameworkDeactivated)));

  driver1.stop();

  AWAIT_READY(frameworkDeactivated);

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.mutable_id()->MergeFrom(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkActivated(_, _));

  EXPECT_CALL(sched2, registered(_, frameworkId, _));

  // Even though the scheduler failed over, the 1 cpu, 256 mem
  // task that it launched earlier should still be running, so
  // only 2 cpus and 768 mem are available.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that if a framework launches a task and then the framework
// is killed, the tasks resources are returned and reoffered correctly.
TYPED_TEST(AllocatorTest, FrameworkExited)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  ExecutorInfo executor1; // Bug in gcc 4.1.*, must assign on next line.
  executor1 = CREATE_EXECUTOR_INFO("executor-1", "exit 1");

  ExecutorInfo executor2; // Bug in gcc 4.1.*, must assign on next line.
  executor2 = CREATE_EXECUTOR_INFO("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  slave::Flags flags = this->CreateSlaveFlags();

  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time the framework is offered resources, all of the
  // cluster's resources should be avaliable.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(executor1, 1, 2, 512, "*"));

  // The framework does not use all the resources.
  Future<Nothing> resourcesRecovered;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesRecovered(&this->allocator),
                    FutureSatisfy(&resourcesRecovered)));

  EXPECT_CALL(exec1, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  // Ensures that framework 1's task is completely launched
  // before we kill the framework to test if its resources
  // are recovered correctly.
  AWAIT_READY(launchTask);

  // We need to wait until the allocator knows about the unused
  // resources to start the second framework so that we get the
  // expected offer.
  AWAIT_READY(resourcesRecovered);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time sched2 gets an offer, framework 1 has a task
  // running with 2 cpus and 512 mem, leaving 1 cpu and 512 mem.
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(LaunchTasks(executor2, 1, 1, 256, "*"));

  // The framework 2 does not use all the resources.
  Future<Nothing> resourcesRecovered2;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesRecovered(&this->allocator),
                    FutureSatisfy(&resourcesRecovered2)));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver2.start();

  AWAIT_READY(launchTask);

  AWAIT_READY(resourcesRecovered2);

  // Shut everything down but check that framework 2 gets the
  // resources from framework 1 after it is shutdown.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(2)); // Once for each framework.

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(2)); // Once for each framework.

  // After we stop framework 1, all of it's resources should
  // have been returned, but framework 2 should still have a
  // task with 1 cpu and 256 mem, leaving 2 cpus and 768 mem.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that if a framework launches a task and then the slave the
// task was running on gets killed, the task's resources are properly
// recovered and, along with the rest of the resources from the killed
// slave, never offered again.
TYPED_TEST(AllocatorTest, SlaveLost)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->StartSlave(&exec, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Initially, all of slave1's resources are available.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 512, "*"));

  Future<Nothing> resourcesRecovered;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesRecovered(&this->allocator),
                    FutureSatisfy(&resourcesRecovered)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  // Ensures the task is completely launched before we kill the
  // slave, to test that the task's and executor's resources are
  // recovered correctly (i.e. never reallocated since the slave
  // is killed).
  AWAIT_READY(launchTask);

  // Framework does not use all the resources.
  AWAIT_READY(resourcesRecovered);

  // 'resourcesRecovered' should be called twice, once for the task
  // and once for the executor.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .Times(2);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoAll(InvokeSlaveRemoved(&this->allocator),
                    FutureSatisfy(&slaveRemoved)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(_, _));

  this->ShutdownSlaves();

  AWAIT_READY(slaveRemoved);

  slave::Flags flags2 = this->CreateSlaveFlags();
  flags2.resources = string("cpus:3;mem:256;disk:1024;ports:[31000-32000]");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // Eventually after slave2 is launched, we should get
  // an offer that contains all of slave2's resources
  // and none of slave1's resources.
  Future<vector<Offer> > resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 256)))
    .WillOnce(FutureArg<1>(&resourceOffers));

  Try<PID<Slave> > slave2 = this->StartSlave(flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers);

  EXPECT_EQ(Resources(resourceOffers.get()[0].resources()),
            Resources::parse(flags2.resources.get()).get());

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that if a slave is added after some allocations have already
// occurred, its resources are added to the available pool of
// resources and offered appropriately.
TYPED_TEST(AllocatorTest, SlaveAdded)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->StartSlave(&exec, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of slave1's resources are avaliable.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 512, "*"));

  // We filter the first time so that the unused resources
  // on slave1 from the task launch won't get reoffered
  // immediately and will get combined with slave2's
  // resources for a single offer.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillOnce(InvokeResourcesRecoveredWithFilters(&this->allocator, 0.1))
    .WillRepeatedly(InvokeResourcesRecoveredWithFilters(&this->allocator, 0));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  slave::Flags flags2 = this->CreateSlaveFlags();
  flags2.resources = Some("cpus:4;mem:2048");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // After slave2 launches, all of its resources are combined with the
  // resources on slave1 that the task isn't using.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(5, 2560)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Try<PID<Slave> > slave2 = this->StartSlave(flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(2));

  this->Shutdown();
}


// Checks that if a task is launched and then finishes normally, its
// resources are recovered and reoffered correctly.
TYPED_TEST(AllocatorTest, TaskFinished)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of the slave's resources.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 2, 1, 256, "*"));

  // Some resources will be unused and we need to make sure that we
  // don't send the TASK_FINISHED status update below until after the
  // allocator knows about the unused resources so that it can
  // aggregate them with the resources from the finished task.
  Future<Nothing> resourcesRecovered;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoAll(InvokeResourcesRecovered(&this->allocator),
                          FutureSatisfy(&resourcesRecovered)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  AWAIT_READY(resourcesRecovered);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _));

  // After the first task gets killed.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that cpus only resources are offered
// and tasks using only cpus are launched.
TYPED_TEST(AllocatorTest, CpusOnlyOfferedAndTaskLaunched)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Start a slave with cpus only resources.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:0");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  // Launch a cpus only task.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 0)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 0, "*"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  // Check that cpus resources of finished task are offered again.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 0)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that memory only resources are offered
// and tasks using only memory are launched.
TYPED_TEST(AllocatorTest, MemoryOnlyOfferedAndTaskLaunched)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Start a slave with memory only resources.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:0;mem:200");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  // Launch a memory only task.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(0, 200)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 0, 200, "*"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  // Check that mem resources of finished task are offered again.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(0, 200)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that a slave that is not whitelisted will not have its
// resources get offered, and that if the whitelist is updated so
// that it is whitelisted, its resources will then be offered.
TYPED_TEST(AllocatorTest, WhitelistSlave)
{
  // Create a dummy whitelist, so that no resources will get allocated.
  string hosts = "dummy-slave";
  string path = "whitelist.txt";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.whitelist = "file://" + path; // TODO(benh): Put in /tmp.

  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Future<Nothing> updateWhitelist1;
  EXPECT_CALL(this->allocator, updateWhitelist(_))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&this->allocator),
                    FutureSatisfy(&updateWhitelist1)));

  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:1024");

  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);
  flags.hostname = hostname.get();

  Try<PID<Slave> > slave = this->StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Once the slave gets whitelisted, all of its resources should be
  // offered to the one framework running.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  // Make sure the allocator has been given the original, empty
  // whitelist.
  AWAIT_READY(updateWhitelist1);

  driver.start();

  // Give the allocator some time to confirm that it doesn't
  // make an allocation.
  Clock::pause();
  Clock::advance(Seconds(1));
  Clock::settle();

  EXPECT_FALSE(resourceOffers.isReady());

  // Update the whitelist to include the slave, so that
  // the allocator will start making allocations.
  hosts = hostname.get() + "\n" + "dummy-slave";

  EXPECT_CALL(this->allocator, updateWhitelist(_));

  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  // Give the WhitelistWatcher some time to notice that
  // the whitelist has changed.
  while (resourceOffers.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }
  Clock::resume();

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();

  os::rm(path);
}


// Checks that a framework attempting to register with an invalid role
// will receive an error message and that roles can be added through the
// master's command line flags.
TYPED_TEST(AllocatorTest, RoleTest)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.roles = Some("role2");
  Try<PID<Master> > master = this->StartMaster(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  // Launch a framework with a role that doesn't exist to see that it
  // receives an error message.
  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_role("role1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkErrorMessage> errorMessage =
    FUTURE_PROTOBUF(FrameworkErrorMessage(), _, _);

  EXPECT_CALL(sched1, error(_, _));

  driver1.start();

  AWAIT_READY(errorMessage);

  // Launch a framework under an existing role to see that it registers.
  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_role("role2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  Future<Nothing> frameworkAdded;
  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _))
    .WillOnce(FutureSatisfy(&frameworkAdded));

  driver2.start();

  AWAIT_READY(registered2);
  AWAIT_READY(frameworkAdded);

  // Shut everything down.
  Future<Nothing> frameworkDeactivated;
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillOnce(FutureSatisfy(&frameworkDeactivated));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver2.stop();
  driver2.join();

  AWAIT_READY(frameworkDeactivated);
  AWAIT_READY(frameworkRemoved);

  driver1.stop();
  driver1.join();

  this->Shutdown();
}


// Checks that in the event of a master failure and the election of a
// new master, if a framework reregisters before a slave that it has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorTest, FrameworkReregistersFirst)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  StandaloneMasterDetector slaveDetector(master.get());

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:1024");

  Try<PID<Slave> > slave = this->StartSlave(&exec, &slaveDetector, flags);
  ASSERT_SOME(slave);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  MockScheduler sched;
  StandaloneMasterDetector schedulerDetector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework should be offered all of the resources on the slave
  // since it is the only framework running.
  EXPECT_CALL(sched, resourceOffers(&driver, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 500, "*"))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.start();

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Make sure the slave handles status update acknowledgement so that
  // it doesn't try to retry the update after master failover.
  AWAIT_READY(_statusUpdateAcknowledgement);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  this->ShutdownMasters();
  this->StopAllocator();

  MockAllocatorProcess<TypeParam> allocator2;

  EXPECT_CALL(allocator2, initialize(_, _, _));

  Future<Nothing> frameworkAdded;
  EXPECT_CALL(allocator2, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator2),
                    FutureSatisfy(&frameworkAdded)));

  EXPECT_CALL(sched, registered(&driver, _, _));

  Try<PID<Master> > master2 = this->StartMaster(&allocator2);
  ASSERT_SOME(master2);

  EXPECT_CALL(sched, disconnected(_));

  // Inform the scheduler about the new master.
  schedulerDetector.appoint(master2.get());

  AWAIT_READY(frameworkAdded);

  EXPECT_CALL(allocator2, slaveAdded(_, _, _));

  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  // Inform the slave about the new master.
  slaveDetector.appoint(master2.get());

  AWAIT_READY(resourceOffers2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

  // Shut everything down.
  EXPECT_CALL(allocator2, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator2, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(allocator2, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(allocator2, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that in the event of a master failure and the election of a
// new master, if a slave reregisters before a framework that has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorTest, SlaveReregistersFirst)
{
  EXPECT_CALL(this->allocator, initialize(_, _, _));

  Try<PID<Master> > master = this->StartMaster(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  StandaloneMasterDetector slaveDetector(master.get());

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:1024");

  Try<PID<Slave> > slave = this->StartSlave(&exec, &slaveDetector, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  StandaloneMasterDetector schedulerDetector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _));

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework should be offered all of the resources on the slave
  // since it is the only framework running.
  EXPECT_CALL(sched, resourceOffers(&driver, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 500, "*"))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.start();

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Make sure the slave handles status update acknowledgement so that
  // it doesn't try to retry the update after master failover.
  AWAIT_READY(_statusUpdateAcknowledgement);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  this->ShutdownMasters();
  this->StopAllocator();

  MockAllocatorProcess<TypeParam> allocator2;

  EXPECT_CALL(allocator2, initialize(_, _, _));

  Future<Nothing> slaveAdded;
  EXPECT_CALL(allocator2, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&allocator2),
                    FutureSatisfy(&slaveAdded)));

  Try<PID<Master> > master2 = this->StartMaster(&allocator2);
  ASSERT_SOME(master2);

  // Inform the slave about the new master.
  slaveDetector.appoint(master2.get());

  AWAIT_READY(slaveAdded);

  EXPECT_CALL(sched, disconnected(_));

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(allocator2, frameworkAdded(_, _, _));

  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  // Inform the scheduler about the new master.
  schedulerDetector.appoint(master2.get());

  AWAIT_READY(resourceOffers2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

  // Shut everything down.
  EXPECT_CALL(allocator2, resourcesRecovered(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator2, frameworkDeactivated(_))
    .Times(AtMost(1));

  EXPECT_CALL(allocator2, frameworkRemoved(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  EXPECT_CALL(allocator2, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}
