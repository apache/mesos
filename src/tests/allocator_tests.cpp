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

#include <mesos/scheduler.hpp>

#include "configurator/configuration.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::ProcessBasedIsolationModule;
using mesos::internal::slave::Slave;

using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::ByRef;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::InSequence;
using testing::Return;
using testing::SaveArg;


void checkResources(vector<Offer> offers, int cpus, int mem)
{
  EXPECT_EQ(1u, offers.size());

  EXPECT_EQ(2, offers[0].resources().size());

  foreach (const Resource& resource, offers[0].resources()) {
    if (resource.name() == "cpus") {
      EXPECT_EQ(resource.scalar().value(), cpus);
    } else if (resource.name() == "mem") {
      EXPECT_EQ(resource.scalar().value(), mem);
    }
  }
}


TEST(AllocatorTest, DRFAllocatorProcess)
{
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  FrameworkID frameworkId1;

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  FrameworkID frameworkId2;

  FrameworkInfo frameworkInfo3;
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_user("user1");
  FrameworkID frameworkId3;

  MockAllocator<TestAllocatorProcess > a;

  EXPECT_CALL(a, initialize(_, _));

  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId1)));

  trigger framework2Added;
  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId2),
		    Trigger(&framework2Added)));

  trigger framework3Added;
  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo3), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId3),
		    Trigger(&framework3Added)));

  EXPECT_CALL(a, frameworkDeactivated(_))
    .Times(3);

  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId1))));

  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId2))));

  trigger lastFrameworkRemoved;
  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId3))))
    .WillOnce(Trigger(&lastFrameworkRemoved));

  SlaveID slaveId4;
  EXPECT_CALL(a, slaveAdded(_, _, _))
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoAll(InvokeSlaveAdded(&a),
		    SaveArg<0>(&slaveId4)));

  EXPECT_CALL(a, slaveRemoved(_))
    .Times(3);

  trigger lastSlaveRemoved;
  EXPECT_CALL(a, slaveRemoved(Eq(ByRef(slaveId4))))
    .WillOnce(Trigger(&lastSlaveRemoved));

  EXPECT_CALL(a, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  Master m(&a);
  PID<Master> master = process::spawn(m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources = Resources::parse("cpus:2;mem:1024");
  Slave s(resources, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s);

  BasicMasterDetector detector(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  vector<Offer> offers1;
  trigger resourceOfferTrigger;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers1),
		    Trigger(&resourceOfferTrigger)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(resourceOfferTrigger);

  EXPECT_THAT(offers1, OfferEq(2, 1024));

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  vector<Offer> offers2, offers3;
  trigger resourceOfferTrigger2, resourceOfferTrigger3;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
		    Trigger(&resourceOfferTrigger2)))
    .WillOnce(DoAll(SaveArg<1>(&offers3),
		    Trigger(&resourceOfferTrigger3)))
    .WillRepeatedly(Return());

  driver2.start();

  WAIT_UNTIL(framework2Added);

  Resources resources2 = Resources::parse("cpus:1;mem:512");
  Slave s2(resources2, true, &isolationModule);
  PID<Slave> slave2 = process::spawn(s2);

  BasicMasterDetector detector2(master, slave2, true);

  WAIT_UNTIL(resourceOfferTrigger2);

  EXPECT_THAT(offers2, OfferEq(1, 512));

  Resources resources3 = Resources::parse("cpus:3;mem:2048");
  Slave s3(resources3, true, &isolationModule);
  PID<Slave> slave3 = process::spawn(s3);
  BasicMasterDetector detector3(master, slave3, true);

  WAIT_UNTIL(resourceOfferTrigger3);

  EXPECT_THAT(offers3, OfferEq(3, 2048));

  MockScheduler sched3;
  MesosSchedulerDriver driver3(&sched3, frameworkInfo3, master);

  EXPECT_CALL(sched3, registered(_, _, _));

  vector<Offer> offers4;
  trigger resourceOfferTrigger4;
  EXPECT_CALL(sched3, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers4),
		    Trigger(&resourceOfferTrigger4)));

  driver3.start();

  WAIT_UNTIL(framework3Added);

  Resources resources4 = Resources::parse("cpus:4;mem:4096");
  Slave s4(resources4, true, &isolationModule);
  PID<Slave> slave4 = process::spawn(s4);
  BasicMasterDetector detector4(master, slave4, true);

  WAIT_UNTIL(resourceOfferTrigger4);

  EXPECT_THAT(offers4, OfferEq(4, 4096));

  driver3.stop();
  driver2.stop();
  driver1.stop();

  WAIT_UNTIL(lastFrameworkRemoved);

  process::terminate(slave1);
  process::wait(slave1);

  process::terminate(slave2);
  process::wait(slave2);

  process::terminate(slave3);
  process::wait(slave3);

  process::terminate(slave4);
  process::wait(slave4);

  WAIT_UNTIL(lastSlaveRemoved);

  process::terminate(master);
  process::wait(master);
}


template <typename T>
class AllocatorTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    process::spawn(allocator.real);
  }

  virtual void TearDown()
  {
    process::terminate(allocator.real);
    process::wait(allocator.real);
  }

  MockAllocator<T> allocator;
};


// Causes all TYPED_TEST(AllocatorTest, ...) to be run for
// each of the specified Allocator classes.
TYPED_TEST_CASE(AllocatorTest, AllocatorTypes);


TYPED_TEST(AllocatorTest, MockAllocator)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  Master m(&this->allocator);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffers;

  vector<Offer> offers;

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&resourceOffers)));

  driver.start();

  WAIT_UNTIL(resourceOffers);

  EXPECT_THAT(offers, OfferEq(2, 1024));

  driver.stop();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, ResourcesUnused)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _))
    .Times(2);

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  trigger resourcesUnusedTrigger;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
		    Trigger(&resourcesUnusedTrigger),
		    Return()))
    .WillRepeatedly(DoDefault());

  // Prevent any resources from being recovered by returning instead
  // of allowing resourcesRecovered to be invoked, so the only ones
  // that are reallocated are those returned by resourcesUnused.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(Return());

  Master m(&(this->allocator));
  PID<Master> master = process::spawn(m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources1 = Resources::parse("cpus:2;mem:1024");
  Slave s(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s);
  BasicMasterDetector(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, 1, 512))
    .WillRepeatedly(DeclineOffers());

  driver1.start();

  WAIT_UNTIL(resourcesUnusedTrigger);

  FrameworkInfo info2;
  info2.set_user("user2");
  info2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, info2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  vector<Offer> offers;
  trigger offered;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&offered)))
    .WillRepeatedly(Return());

  driver2.start();

  WAIT_UNTIL(offered);

  EXPECT_THAT(offers, OfferEq(1, 512));

  driver1.stop();
  driver2.stop();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


// Tests the situation where a frameworkRemoved call is dispatched
// while we're doing an allocation to that framework, so that
// resourcesRecovered is called for an already removed framework.
TYPED_TEST(AllocatorTest, OutOfOrderDispatch)
{
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  FrameworkID frameworkId1;

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  FrameworkID frameworkId2;

  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
		    SaveArg<0>(&frameworkId1)));

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
		    SaveArg<0>(&frameworkId2)));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  trigger frameworkRemoved, frameworkRemoved2;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(ByRef(frameworkId1))))
    .WillOnce(DoAll(InvokeFrameworkRemoved(&this->allocator),
		    Trigger(&frameworkRemoved),
		    Return()));

  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(ByRef(frameworkId2))))
    .WillOnce(Trigger(&frameworkRemoved2));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources savedResources;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    // "Catches" the resourcesRecovered call from the master, so
    // that it doesn't get processed until we redispatch it after
    // the frameworkRemoved trigger.
    .WillOnce(DoAll(SaveArg<0>(&frameworkId),
		    SaveArg<1>(&slaveId),
		    SaveArg<2>(&savedResources)))
    .WillRepeatedly(DoDefault());

  Master m(&(this->allocator));
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo1, master);

  trigger offered;
  vector<Offer> offers;

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&offered)));

  driver.start();

  WAIT_UNTIL(offered);

  EXPECT_THAT(offers, OfferEq(2, 1024));

  driver.stop();
  driver.join();

  WAIT_UNTIL(frameworkRemoved);

  // Re-dispatch the resourcesRecovered call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  dispatch(this->allocator, &AllocatorProcess::resourcesRecovered,
	   frameworkId, slaveId, savedResources);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  trigger offered2;
  vector<Offer> offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
		    Trigger(&offered2)));

  driver2.start();

  WAIT_UNTIL(offered2);

  EXPECT_THAT(offers2, OfferEq(2, 1024));

  driver2.stop();
  driver2.join();

  WAIT_UNTIL(frameworkRemoved2);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, SchedulerFailover)
{
  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, Eq(UnregisterFrameworkMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, frameworkActivated(_, _));

  trigger frameworkDeactivatedTrigger;
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillOnce(DoAll(InvokeFrameworkDeactivated(&this->allocator),
		    Trigger(&frameworkDeactivatedTrigger)))
    .WillOnce(DoDefault());

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  trigger resourcesRecoveredTrigger;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  // We don't filter the unused resources to make sure that
  // they get offered to the framework as soon as it fails over.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  Master m(&this->allocator);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  trigger launchTaskTrigger;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(Trigger(&launchTaskTrigger));

  EXPECT_CALL(exec, shutdown(_));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  EXPECT_CALL(isolationModule, resourcesChanged(_, _, _));

  Resources resources = Resources::parse("cpus:3;mem:1024");
  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);
  BasicMasterDetector detector(master, slave, true);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_failover_timeout(.5);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  FrameworkID frameworkId;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  // We don't know how many times the allocator might try to
  // allocate to the failing framework before it get killed.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all cluster resources are avaliable.
  trigger resourceOffersTrigger1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
		    Trigger(&resourceOffersTrigger1)));

  driver1.start();

  WAIT_UNTIL(resourceOffersTrigger1);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  EXPECT_CALL(sched2, registered(_, frameworkId, _));

  // Even though the scheduler failed over, the 1 cpu, 512 mem
  // task that it launched earlier should still be running, so
  // only 2 cpus and 768 mem are available.
  trigger resourceOffersTrigger2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(Trigger(&resourceOffersTrigger2));

  // Ensures that the task has been completely launched
  // before we have the framework fail over.
  WAIT_UNTIL(launchTaskTrigger);

  driver1.stop();

  WAIT_UNTIL(frameworkDeactivatedTrigger);

  driver2.start();

  WAIT_UNTIL(resourceOffersTrigger2);

  driver2.stop();
  driver2.join();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);

  process::filter(NULL);
}


TYPED_TEST(AllocatorTest, FrameworkExited)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _))
    .Times(2);

  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .Times(2);

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillRepeatedly(DoDefault());

  Master m(&this->allocator);
  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(2);

  trigger launchTaskTrigger;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(Trigger(&launchTaskTrigger))
    .WillOnce(DoDefault());

  trigger shutdownTrigger;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(DoDefault())
    .WillOnce(Trigger(&shutdownTrigger));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);
  EXPECT_CALL(isolationModule, resourcesChanged(_, _, _))
    .Times(2);

  Resources resources1 = Resources::parse("cpus:3;mem:1024");
  Slave s1(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s1);
  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time the framework is offered resources,
  // all of the cluster's resources should be avaliable.
  trigger resourceOffersTrigger1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
		    Trigger(&resourceOffersTrigger1)));

  driver1.start();

  WAIT_UNTIL(resourceOffersTrigger1);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time sched2 gets an offer, framework 1 has a
  // task running with 2 cpus and 512 mem, leaving 1 cpu and
  // 512 mem.
  trigger resourceOffersTrigger2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
		    Trigger(&resourceOffersTrigger2)));

  // After we kill framework 1, all of it's resources should
  // have been returned, but framework 2 should still have a
  // task with 1 cpu and 256 mem, leaving 2 cpus and 768 mem.
  trigger resourceOffersTrigger3;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(Trigger(&resourceOffersTrigger3));

  driver2.start();

  WAIT_UNTIL(resourceOffersTrigger2);

  // Ensures that framework 1's task is completely launched
  // before we kill the framework to test if its resources
  // are recovered correctly.
  WAIT_UNTIL(launchTaskTrigger);

  driver1.stop();
  driver1.join();

  WAIT_UNTIL(resourceOffersTrigger3);

  driver2.stop();
  driver2.join();

  WAIT_UNTIL(shutdownTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, SlaveLost)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _))
    .Times(2);

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  trigger slaveRemovedTrigger1, slaveRemovedTrigger2;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoAll(InvokeSlaveRemoved(&this->allocator),
		    Trigger(&slaveRemovedTrigger1)))
    .WillOnce(Trigger(&slaveRemovedTrigger2));

  Master m(&this->allocator);
  PID<Master> master = process::spawn(m);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  trigger launchTaskTrigger;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
		    Trigger(&launchTaskTrigger)));

  EXPECT_CALL(exec, shutdown(_));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  EXPECT_CALL(isolationModule, resourcesChanged(_, _, _));

  Resources resources1 = Resources::parse("cpus:2;mem:1024");
  Slave s1(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s1);
  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, slaveLost(_, _));

  trigger resourceOffersTrigger1, resourceOffersTrigger2;
  {
    // Ensures that the following EXPEC_CALLs happen in order.
    InSequence dummy;

    // Initially, all of slave1's resources are avaliable.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
      .WillOnce(DoAll(LaunchTasks(1, 2, 512),
		      Trigger(&resourceOffersTrigger1)));

    // Eventually after slave2 is launched, we should get
    // an offer that contains all of slave2's resources
    // and none of slave1's resources.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 256)))
      .WillOnce(Trigger(&resourceOffersTrigger2));
  }

  driver1.start();

  WAIT_UNTIL(resourceOffersTrigger1);

  // Ensures the task is completely launched before we
  // kill the slave, to test that the task's resources
  // are recovered correctly (i.e. never reallocated
  // since the slave is killed)
  WAIT_UNTIL(launchTaskTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  WAIT_UNTIL(slaveRemovedTrigger1);

  ProcessBasedIsolationModule isolationModule2;
  Resources resources2 = Resources::parse("cpus:3;mem:256");
  Slave s2(resources2, true, &isolationModule2);
  PID<Slave> slave2 = process::spawn(s2);
  BasicMasterDetector detector2(master, slave2, true);

  WAIT_UNTIL(resourceOffersTrigger2);

  driver1.stop();
  driver1.join();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave2);
  process::wait(slave2);

  WAIT_UNTIL(slaveRemovedTrigger2);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, SlaveAdded)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _))
    .Times(2);

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(Trigger(&slaveRemovedTrigger));

  // We filter the first time so that the unused resources
  // on slave1 from the task launch won't get reoffered
  // immediately and will get combined with slave2's
  // resources for a single offer.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, .1))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  Master m(&this->allocator);
  PID<Master> master = process::spawn(m);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  trigger launchTaskTrigger;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
		    Trigger(&launchTaskTrigger)));

  trigger shutdownTrigger;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownTrigger));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  EXPECT_CALL(isolationModule, resourcesChanged(_, _, _));

  Resources resources1 = Resources::parse("cpus:3;mem:1024");
  Slave s1(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s1);
  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  trigger resourceOffersTrigger1, resourceOffersTrigger2;
  {
    // Ensures that the following EXPEC_CALLs happen in order.
    InSequence dummy;

    // Initially, all of slave1's resources are avaliable.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
      .WillOnce(DoAll(LaunchTasks(1, 2, 512),
		      Trigger(&resourceOffersTrigger1)));

    // After slave2 launches, all of its resources are
    // combined with the resources on slave1 that the
    // task isn't using.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(5, 2560)))
      .WillOnce(Trigger(&resourceOffersTrigger2));
  }

  driver1.start();

  WAIT_UNTIL(resourceOffersTrigger1);

  // Wait until the filter from resourcesUnused above times
  // out so that all resources not used by the launched task
  // will get offered together. TODO(tmarshall): replace this
  // with a Clock::advance().
  sleep(1);

  WAIT_UNTIL(launchTaskTrigger);

  Resources resources2 = Resources::parse("cpus:4;mem:2048");
  Slave s2(resources2, true, &isolationModule);
  PID<Slave> slave2 = process::spawn(s2);
  BasicMasterDetector detector2(master, slave2, true);

  WAIT_UNTIL(resourceOffersTrigger2);

  driver1.stop();
  driver1.join();

  WAIT_UNTIL(frameworkRemovedTrigger);

  WAIT_UNTIL(shutdownTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  process::terminate(slave2);
  process::wait(slave2);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, TaskFinished)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, frameworkRemoved(_));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  // The first time we don't filter because we want to see
  // the unused resources from the task launch get reoffered
  // to us, but when that offer is returned unused we do
  // filter so that they won't get reoffered again and will
  // instead get combined with the recovered resources from
  // the task finishing for one offer.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, 0))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 1));

  Master m(&this->allocator);
  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  trigger launchTaskTrigger;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
		    SaveArg<1>(&taskInfo),
		    SendStatusUpdateFromTask(TASK_RUNNING),
		    Trigger(&launchTaskTrigger)))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownTrigger;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownTrigger));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);
  EXPECT_CALL(isolationModule, resourcesChanged(_, _, _))
    .Times(2);

  Resources resources1 = Resources::parse("cpus:3;mem:1024");
  Slave s1(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s1);
  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  trigger resourceOffersTrigger1, resourceOffersTrigger2;
  {
    // Ensures that the following EXPEC_CALLs happen in order.
    InSequence dummy;

    // Initially, all of the slave's resources.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
      .WillOnce(LaunchTasks(2, 1, 256));

    // After the tasks are launched.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 512)))
      .WillOnce(DoAll(DeclineOffers(),
		      Trigger(&resourceOffersTrigger1)));

    // After the first task gets killed.
    EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 768)))
      .WillOnce(Trigger(&resourceOffersTrigger2));
  }

  driver1.start();

  WAIT_UNTIL(resourceOffersTrigger1);

  WAIT_UNTIL(launchTaskTrigger);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(status);

  WAIT_UNTIL(resourceOffersTrigger2);

  driver1.stop();
  driver1.join();

  WAIT_UNTIL(shutdownTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, WhitelistSlave)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, frameworkRemoved(_));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  trigger updateWhitelistTrigger1, updateWhitelistTrigger2;
  EXPECT_CALL(this->allocator, updateWhitelist(_))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&this->allocator),
		    Trigger(&updateWhitelistTrigger1)))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&this->allocator),
		    Trigger(&updateWhitelistTrigger2)));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  // Create a dummy whitelist, so that no resources will get allocated.
  string hosts = "dummy-slave";
  string path = "whitelist.txt";
  CHECK (os::write(path, hosts).isSome()) << "Error writing whitelist";

  flags::Flags<logging::Flags, master::Flags> flags;
  flags.whitelist = "file://" + path; // TODO(benh): Put in /tmp.
  Master m(&this->allocator, flags);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);
  Resources resources = Resources::parse("cpus:2;mem:1024");
  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersTrigger;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(Trigger(&resourceOffersTrigger));

  WAIT_UNTIL(updateWhitelistTrigger1);

  driver.start();

  // Give the allocator some time to confirm that it doesn't
  // make an allocation.
  sleep(1);
  EXPECT_FALSE(resourceOffersTrigger.value);

  // Update the whitelist to include the slave, so that
  // the allocator will start making allocations.
  Try<string> hostname = os::hostname();
  ASSERT_TRUE(hostname.isSome());
  hosts = hostname.get() + "\n" + "dummy-slave";
  CHECK (os::write(path, hosts).isSome()) << "Error writing whitelist";

  // Give the WhitelistWatcher some time to notice that
  // the whitelist has changed.
  sleep(4);

  WAIT_UNTIL(updateWhitelistTrigger2);

  WAIT_UNTIL(resourceOffersTrigger);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);

  os::rm(path);
}
