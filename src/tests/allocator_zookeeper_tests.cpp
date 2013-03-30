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

#include <process/gmock.hpp>

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/master.hpp"

#include "tests/zookeeper_test.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Return;
using testing::SaveArg;


template <typename T = AllocatorProcess>
class AllocatorZooKeeperTest : public ZooKeeperTest
{
public:
  virtual void SetUp()
  {
    ZooKeeperTest::SetUp();

    a1 = new Allocator(&allocator1);
    a2 = new Allocator(&allocator2);
  }

  virtual void TearDown()
  {
    ZooKeeperTest::TearDown();

    delete a1;
    delete a2;
  }

protected:
  T allocator1;
  MockAllocatorProcess<T> allocator2;
  Allocator* a1;
  Allocator* a2;
};


// Runs TYPED_TEST(AllocatorZooKeeperTest, ...) on all AllocatorTypes.
TYPED_TEST_CASE(AllocatorZooKeeperTest, AllocatorTypes);


// Checks that in the event of a master failure and the election of a
// new master, if a framework reregisters before a slave that it has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorZooKeeperTest, FrameworkReregistersFirst)
{
  EXPECT_CALL(this->allocator2, initialize(_, _));

  trigger frameworkAddedCall;
  EXPECT_CALL(this->allocator2, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator2),
                    Trigger(&frameworkAddedCall)));

  EXPECT_CALL(this->allocator2, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator2, frameworkRemoved(_));

  EXPECT_CALL(this->allocator2, slaveAdded(_, _, _));

  trigger slaveRemovedCall;
  EXPECT_CALL(this->allocator2, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedCall));

  EXPECT_CALL(this->allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  // Stop the failing master from telling the slave to shut down when
  // it is killed.
  trigger shutdownMsg;
  EXPECT_MESSAGE(Eq(ShutdownMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&shutdownMsg),
			  Return(true)));

  // Stop the slave from reregistering with the new master until the
  // framework has reregistered.
  EXPECT_MESSAGE(Eq(ReregisterSlaveMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  Files files;
  Master m(this->a1, &files);
  PID<Master> master1 = process::spawn(&m);

  string zk = "zk://" + this->server->connectString() + "/znode";
  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master1, true, true);
  ASSERT_SOME(detector);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  // By default, slaves in tests have cpus=2, mem=1024.
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  Try<MasterDetector*> slaveDetector =
    MasterDetector::create(zk, slave, false, true);
  ASSERT_SOME(slaveDetector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO,zk);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  vector<Offer> offers, offers2;
  trigger resourceOffersCall, resourceOffersCall2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    LaunchTasks(1, 1, 512),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers2),
			  Trigger(&resourceOffersCall2)));

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
		    Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // The framework will be offered all of the resources on the slave,
  // since it is the only framework running.
  EXPECT_THAT(offers, OfferEq(2, 1024));

  Resources launchedResources = Resources::parse("cpus:1;mem:512");
  trigger resourcesChangedCall;
  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(launchedResources)))
    .WillOnce(Trigger(&resourcesChangedCall));

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedCall);

  process::terminate(master1);
  process::wait(master1);
  MasterDetector::destroy(detector.get());

  WAIT_UNTIL(shutdownMsg);

  Files files2;
  Master m2(this->a2, &files2);
  PID<Master> master2 = process::spawn(m2);

  Try<MasterDetector*> detector2 =
    MasterDetector::create(zk, master2, true, true);
  ASSERT_SOME(detector2);

  WAIT_UNTIL(frameworkAddedCall);

  // This ensures that even if the allocator made multiple allocations
  // before the master was killed, we wait until the first allocation
  // after the slave reregisters with the new master before checking
  // that the allocation is correct.
  resourceOffersCall2.value = false;

  // We kill the filter so that ReregisterSlaveMessages can get
  // to the master now that the framework has been added, ensuring
  // that the slave reregisters after the framework.
  process::filter(NULL);

  WAIT_UNTIL(resourceOffersCall2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(offers2, OfferEq(1, 512));

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);
  MasterDetector::destroy(slaveDetector.get());

  WAIT_UNTIL(slaveRemovedCall);

  process::terminate(master2);
  process::wait(master2);
  MasterDetector::destroy(detector2.get());
}


// Checks that in the event of a master failure and the election of a
// new master, if a slave reregisters before a framework that has
// resources on it reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorZooKeeperTest, SlaveReregisterFirst)
{
  EXPECT_CALL(this->allocator2, initialize(_, _));

  EXPECT_CALL(this->allocator2, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator2, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator2, frameworkRemoved(_));

  trigger slaveAddedCall;
  EXPECT_CALL(this->allocator2, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&this->allocator2),
                    Trigger(&slaveAddedCall)));

  trigger slaveRemovedCall;
  EXPECT_CALL(this->allocator2, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedCall));

  EXPECT_CALL(this->allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  // Stop the failing master from telling the slave to shut down when
  // it is killed.
  trigger shutdownMsg;
  EXPECT_MESSAGE(Eq(ShutdownMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&shutdownMsg),
                          Return(true)));

  // Stop the framework from reregistering with the new master until
  // the slave has reregistered.
  EXPECT_MESSAGE(Eq(ReregisterFrameworkMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  Files files;
  Master m(this->a1, &files);
  PID<Master> master1 = process::spawn(&m);

  string zk = "zk://" + this->server->connectString() + "/znode";
  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master1, true, true);
  ASSERT_SOME(detector);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  // By default, slaves in tests have cpus=2, mem=1024.
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  Try<MasterDetector*> slaveDetector =
    MasterDetector::create(zk, slave, false, true);
  ASSERT_SOME(slaveDetector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO,zk);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  vector<Offer> offers, offers2;
  trigger resourceOffersCall, resourceOffersCall2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    LaunchTasks(1, 1, 512),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers2),
                          Trigger(&resourceOffersCall2)));

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
		                Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // The framework will be offered all of the resources on the slave,
  // since it is the only framework running.
  EXPECT_THAT(offers, OfferEq(2, 1024));

  Resources launchedResources = Resources::parse("cpus:1;mem:512");
  trigger resourcesChangedCall;
  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(launchedResources)))
    .WillOnce(Trigger(&resourcesChangedCall));

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedCall);

  process::terminate(master1);
  process::wait(master1);
  MasterDetector::destroy(detector.get());

  WAIT_UNTIL(shutdownMsg);

  Files files2;
  Master m2(this->a2, &files2);
  PID<Master> master2 = process::spawn(m2);

  Try<MasterDetector*> detector2 =
    MasterDetector::create(zk, master2, true, true);
  ASSERT_SOME(detector2);

  WAIT_UNTIL(slaveAddedCall);

  // This ensures that even if the allocator made multiple allocations
  // before the master was killed, we wait until the first allocation
  // after the framework reregisters with the new master before
  // checking that the allocation is correct.
  resourceOffersCall2.value = false;

  // We kill the filter so that ReregisterFrameworkMessages can get
  // to the master now that the slave has been added, ensuring
  // that the framework reregisters after the slave.
  process::filter(NULL);

  WAIT_UNTIL(resourceOffersCall2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(offers2, OfferEq(1, 512));

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);
  MasterDetector::destroy(slaveDetector.get());

  WAIT_UNTIL(slaveRemovedCall);

  process::terminate(master2);
  process::wait(master2);
  MasterDetector::destroy(detector2.get());
}
