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

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include "configurator/configuration.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/process_isolator.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::ProcessIsolator;
using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::ByRef;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::InSequence;
using testing::Return;
using testing::SaveArg;


class DRFAllocatorTest : public MesosTest
{};


// Checks that the DRF allocator implements the DRF algorithm
// correctly.
TEST_F(DRFAllocatorTest, DRFAllocatorProcess)
{
  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(m);

  TestingIsolator isolator;
  setSlaveResources("cpus:2;mem:1024;disk:0");
  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s);
  // Total cluster resources now cpus=2, mem=1024.
  BasicMasterDetector detector(master, slave1, true);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_UNTIL(offers1);

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far, giving it cpus=2, mem=1024.
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));
  // framework1 share = 1

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  driver2.start();

  setSlaveResources("cpus:1;mem:512;disk:0");
  Slave s2(slaveFlags, true, &isolator, &files);
  PID<Slave> slave2 = process::spawn(s2);
  // Total cluster resources now cpus=3, mem=1536.
  // framework1 share = 0.66
  // framework2 share = 0

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  BasicMasterDetector detector2(master, slave2, true);

  AWAIT_UNTIL(offers2);

  // framework2 will be offered all of slave2's resources since
  // it has the lowest share, giving it cpus=1, mem=512.
  EXPECT_THAT(offers2.get(), OfferEq(1, 512));
  // framework1 share =  0.66
  // framework2 share = 0.33

  setSlaveResources("cpus:3;mem:2048;disk:0");
  Slave s3(slaveFlags, true, &isolator, &files);
  PID<Slave> slave3 = process::spawn(s3);

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3));

  BasicMasterDetector detector3(master, slave3, true);
  // Total cluster resources now cpus=6, mem=3584.
  // framework1 share = 0.33
  // framework2 share = 0.16

  AWAIT_UNTIL(offers3);

  // framework2 will be offered all of slave3's resources since
  // it has the lowest share, giving it cpus=4, mem=2560.
  EXPECT_THAT(offers3.get(), OfferEq(3, 2048));
  // framework1 share = 0.33
  // framework2 share = 0.71

  FrameworkInfo frameworkInfo3;
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_user("user1");
  MockScheduler sched3;
  MesosSchedulerDriver driver3(&sched3, frameworkInfo3, master);

  EXPECT_CALL(sched3, registered(_, _, _));

  driver3.start();

  setSlaveResources("cpus:4;mem:4096;disk:0");
  Slave s4(slaveFlags, true, &isolator, &files);
  PID<Slave> slave4 = process::spawn(s4);

  Future<vector<Offer> > offers4;
  EXPECT_CALL(sched3, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers4));

  BasicMasterDetector detector4(master, slave4, true);
  // Total cluster resources now cpus=10, mem=7680.
  // framework1 share = 0.2
  // framework2 share = 0.4
  // framework3 share = 0

  AWAIT_UNTIL(offers4);

  // framework3 will be offered all of slave4's resources since
  // it has the lowest share.
  EXPECT_THAT(offers4.get(), OfferEq(4, 4096));

  driver1.stop();
  driver2.stop();
  driver3.stop();

  process::terminate(slave1);
  process::wait(slave1);

  process::terminate(slave2);
  process::wait(slave2);

  process::terminate(slave3);
  process::wait(slave3);

  process::terminate(slave4);
  process::wait(slave4);

  process::terminate(master);
  process::wait(master);
}


template <typename T>
class AllocatorTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();
    a = new Allocator(&allocator);
  }

  virtual void TearDown()
  {
    delete a;
    MesosTest::TearDown();
  }

  MockAllocatorProcess<T> allocator;
  Allocator* a;
};


// Causes all TYPED_TEST(AllocatorTest, ...) to be run for
// each of the specified Allocator classes.
TYPED_TEST_CASE(AllocatorTest, AllocatorTypes);


// Checks that in a cluster with one slave and one framework, all of
// the slave's resources are offered to the framework.
TYPED_TEST(AllocatorTest, MockAllocator)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  // By default, slaves in tests have cpus=2, mem=1024.
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_UNTIL(offers);

  // The framework should be offered all of the resources on the slave
  // since it is the only framework in the cluster.
  EXPECT_THAT(offers.get(), OfferEq(2, 1024));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillRepeatedly(DoDefault());

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver.stop();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave);
  process::wait(slave);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Checks that when a task is launched with fewer resources than what
// the offer was for, the resources that are returned unused are
// reoffered appropriately.
TYPED_TEST(AllocatorTest, ResourcesUnused)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  // By default, slaves in tests have cpus=2, mem=1024.
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  Future<Nothing> resourcesUnused;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
                    FutureSatisfy(&resourcesUnused)));

  EXPECT_CALL(sched1, registered(_, _, _));

  // The first offer will contain all of the slave's resources, since
  // this is the only framework running so far. Launch a task that
  // uses less than that to leave some resources unused.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(1, 1, 512))
    .WillRepeatedly(DeclineOffers());

  // We don't wait for the task to be launched, since we only care
  // that the offer is accepted, so we don't wait for the executor
  // to recieve messages, but it may get them anyways.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(AtMost(1));

  driver1.start();

  AWAIT_UNTIL(resourcesUnused);

  FrameworkInfo info2;
  info2.set_user("user2");
  info2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, info2, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  AWAIT_UNTIL(offers);

  // framework2 will be offered all of the resources on the slave not
  // being used by the task that was launched.
  EXPECT_THAT(offers.get(), OfferEq(1, 512));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave1);
  process::wait(slave1);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Tests the situation where a frameworkRemoved call is dispatched
// while we're doing an allocation to that framework, so that
// resourcesRecovered is called for an already removed framework.
TYPED_TEST(AllocatorTest, OutOfOrderDispatch)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(&m);

  TestingIsolator isolator;
  // By default, slaves in tests have cpus=2, mem=1024.
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector(master, slave, true);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  FrameworkID frameworkId1;
  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
                    SaveArg<0>(&frameworkId1)));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_UNTIL(offers1);

  // framework1 will be offered all of the slave's resources, since
  // it is the only framework running right now.
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));

  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources savedResources;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
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

  AWAIT_UNTIL(frameworkRemoved);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillOnce(DoDefault());

  // Re-dispatch the resourcesRecovered call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  this->a->resourcesRecovered(frameworkId, slaveId, savedResources);

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  FrameworkID frameworkId2;
  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
                    SaveArg<0>(&frameworkId2)));

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  driver2.start();

  AWAIT_UNTIL(offers2);

  // framework2 will be offered all of the slave's resources, since
  // it is the only framework running right now.
  EXPECT_THAT(offers2.get(), OfferEq(2, 1024));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved2;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(frameworkId2)))
    .WillOnce(FutureSatisfy(&frameworkRemoved2));

  driver2.stop();
  driver2.join();

  AWAIT_UNTIL(frameworkRemoved2);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave);
  process::wait(slave);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Checks that if a framework launches a task and then fails over to a
// new scheduler, the task's resources are not reoffered as long as it
// is running.z
TYPED_TEST(AllocatorTest, SchedulerFailover)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  this->setSlaveResources("cpus:3;mem:1024");
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector(master, slave, true);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_failover_timeout(.5);
  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // We don't filter the unused resources to make sure that
  // they get offered to the framework as soon as it fails over.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, 0));

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
                    FutureArg<1>(&offers1)))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  AWAIT_UNTIL(offers1);

  // Initially, all cluster resources are avaliable.
  EXPECT_THAT(offers1.get(), OfferEq(3, 1024));

  // Ensures that the task has been completely launched
  // before we have the framework fail over.
  AWAIT_UNTIL(launchTask);

  // When we shut down the first framework, we don't want it to tell
  // the master it's shutting down so that the master will wait to see
  // if it fails over.
  DROP_MESSAGES(Eq(UnregisterFrameworkMessage().GetTypeName()), _, _);

  Future<Nothing> frameworkDeactivated;
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillOnce(DoAll(InvokeFrameworkDeactivated(&this->allocator),
                    FutureSatisfy(&frameworkDeactivated)));

  driver1.stop();

  AWAIT_UNTIL(frameworkDeactivated);

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);
  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, framework2, master);

  EXPECT_CALL(this->allocator, frameworkActivated(_, _));

  EXPECT_CALL(sched2, registered(_, frameworkId, _));

  // Even though the scheduler failed over, the 1 cpu, 512 mem
  // task that it launched earlier should still be running, so
  // only 2 cpus and 768 mem are available.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  driver2.start();

  AWAIT_UNTIL(resourceOffers2);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave);
  process::wait(slave);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Checks that if a framework launches a task and then the framework
// is killed, the tasks resources are returned and reoffered correctly.
TYPED_TEST(AllocatorTest, FrameworkExited)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(2);

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask))
    .WillOnce(DoDefault());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(2));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  this->setSlaveResources("cpus:3;mem:1024");
  Slave s1(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s1);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  Future<Nothing> resourcesUnused;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
                    FutureSatisfy(&resourcesUnused)));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time the framework is offered resources,
  // all of the cluster's resources should be avaliable.
  Future<Nothing> resourcesOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureSatisfy(&resourcesOffers1)));

  driver1.start();

  AWAIT_UNTIL(resourcesOffers1);

  AWAIT_UNTIL(resourcesUnused);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time sched2 gets an offer, framework 1 has a
  // task running with 2 cpus and 512 mem, leaving 1 cpu and 512 mem.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
                    FutureSatisfy(&resourceOffers2)));

  driver2.start();

  AWAIT_UNTIL(resourceOffers2);

  // Ensures that framework 1's task is completely launched
  // before we kill the framework to test if its resources
  // are recovered correctly.
  AWAIT_UNTIL(launchTask);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, frameworkRemoved(_));

  // After we kill framework 1, all of it's resources should
  // have been returned, but framework 2 should still have a
  // task with 1 cpu and 256 mem, leaving 2 cpus and 768 mem.
  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  driver1.stop();
  driver1.join();

  AWAIT_UNTIL(resourceOffers3);

  // Shut everything down.
  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver2.stop();
  driver2.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave1);
  process::wait(slave1);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Checks that if a framework launches a task and then the slave the
// task was running on gets killed, the task's resources are properly
// recovered and, along with the rest of the resources from the killed
// slave, never offered again.
TYPED_TEST(AllocatorTest, SlaveLost)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  // By default, slaves in tests have cpus=2, mem=512.
  Slave s1(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s1);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  Future<vector<Offer> > resourceOffers1;
  // Initially, all of slave1's resources are avaliable.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureArg<1>(&resourceOffers1)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_UNTIL(resourceOffers1);

  EXPECT_THAT(resourceOffers1.get(), OfferEq(2, 1024));

  // Ensures the task is completely launched before we
  // kill the slave, to test that the task's resources
  // are recovered correctly (i.e. never reallocated
  // since the slave is killed)
  AWAIT_UNTIL(launchTask);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  Future<Nothing> slaveRemoved1;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoAll(InvokeSlaveRemoved(&this->allocator),
                    FutureSatisfy(&slaveRemoved1)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, slaveLost(_, _));

  process::terminate(slave1);
  process::wait(slave1);

  AWAIT_UNTIL(slaveRemoved1);

  TestingIsolator isolator2(DEFAULT_EXECUTOR_ID, &exec);

  this->setSlaveResources("cpus:3;mem:256");
  Slave s2(this->slaveFlags, true, &isolator2, &files);
  PID<Slave> slave2 = process::spawn(s2);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // Eventually after slave2 is launched, we should get
  // an offer that contains all of slave2's resources
  // and none of slave1's resources.
  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 256)))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  BasicMasterDetector detector2(master, slave2, true);

  AWAIT_UNTIL(resourceOffers2);

  EXPECT_THAT(resourceOffers2.get(), OfferEq(3, 256));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver1.stop();
  driver1.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved2;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved2));

  process::terminate(slave2);
  process::wait(slave2);

  AWAIT_UNTIL(slaveRemoved2);

  process::terminate(master);
  process::wait(master);
}


// Checks that if a slave is added after some allocations have already
// occurred, its resources are added to the available pool of
// resources and offered appropriately.
TYPED_TEST(AllocatorTest, SlaveAdded)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  this->setSlaveResources("cpus:3;mem:1024");
  Slave s1(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s1);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // We filter the first time so that the unused resources
  // on slave1 from the task launch won't get reoffered
  // immediately and will get combined with slave2's
  // resources for a single offer.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, .1))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of slave1's resources are avaliable.
  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureSatisfy(&resourceOffers1)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_UNTIL(resourceOffers1);

  AWAIT_UNTIL(launchTask);

  this->setSlaveResources("cpus:4;mem:2048");
  Slave s2(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave2 = process::spawn(s2);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // After slave2 launches, all of its resources are
  // combined with the resources on slave1 that the
  // task isn't using.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(5, 2560)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  BasicMasterDetector detector2(master, slave2, true);

  AWAIT_UNTIL(resourceOffers2);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave1);
  process::wait(slave1);

  process::terminate(slave2);
  process::wait(slave2);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
}


// Checks that if a task is launched and then finishes normally, its
// resources are recovered and reoffered correctly.
TYPED_TEST(AllocatorTest, TaskFinished)
{
  Files files;
  Master m(this->a, &files);

  EXPECT_CALL(this->allocator, initialize(_, _));

  PID<Master> master = process::spawn(m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  this->setSlaveResources("cpus:3;mem:1024");
  Slave s1(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave1 = process::spawn(s1);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector1(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // The first time we don't filter because we want to see
  // the unused resources from the task launch get reoffered
  // to us, but when that offer is returned unused we do
  // filter so that they won't get reoffered again and will
  // instead get combined with the recovered resources from
  // the task finishing for one offer.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of the slave's resources.
  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(2, 1, 256),
                    FutureSatisfy(&resourceOffers1)));

  // After the tasks are launched.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(DoAll(DeclineOffers(),
                    FutureSatisfy(&resourceOffers2)));

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

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_UNTIL(resourceOffers1);

  AWAIT_UNTIL(launchTask);

  AWAIT_UNTIL(resourceOffers2);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  // After the first task gets killed.
  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  execDriver->sendStatusUpdate(status);

  AWAIT_UNTIL(resourceOffers3);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave1);
  process::wait(slave1);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);
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

  Files files;
  master::Flags masterFlags;
  masterFlags.whitelist = "file://" + path; // TODO(benh): Put in /tmp.
  Master m(this->a, &files, masterFlags);

  EXPECT_CALL(this->allocator, initialize(_, _));

  Future<Nothing> updateWhitelist1;
  EXPECT_CALL(this->allocator, updateWhitelist(_))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&this->allocator),
                    FutureSatisfy(&updateWhitelist1)));

  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Once the slave gets whitelisted, all of its resources should be
  // offered to the one framework running.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  // Make sure the allocator has been given the original, empty
  // whitelist.
  AWAIT_UNTIL(updateWhitelist1);

  driver.start();

  // Give the allocator some time to confirm that it doesn't
  // make an allocation.
  Clock::pause();
  Clock::advance(Seconds(1));
  Clock::settle();

  EXPECT_FALSE(resourceOffers.isReady());

  // Update the whitelist to include the slave, so that
  // the allocator will start making allocations.
  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);
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
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  process::terminate(slave);
  process::wait(slave);

  AWAIT_UNTIL(slaveRemoved);

  process::terminate(master);
  process::wait(master);

  os::rm(path);
}
