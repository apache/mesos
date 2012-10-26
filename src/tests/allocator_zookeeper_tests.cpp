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

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/master.hpp"

#include "tests/zookeeper_test.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::_;
using testing::Return;
using testing::SaveArg;


template <typename T = AllocatorProcess>
class AllocatorZooKeeperTest : public ZooKeeperTest
{
protected:
  T allocator1;
  MockAllocator<T> allocator2;
};


// Runs TYPED_TEST(AllocatorZooKeeperTest, ...) on all AllocatorTypes.
TYPED_TEST_CASE(AllocatorZooKeeperTest, AllocatorTypes);


TYPED_TEST(AllocatorZooKeeperTest, FrameworkReregistersFirst)
{
  EXPECT_CALL(this->allocator2, initialize(_, _));

  trigger frameworkAddedTrigger;
  EXPECT_CALL(this->allocator2, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator2),
		    Trigger(&frameworkAddedTrigger)));

  EXPECT_CALL(this->allocator2, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator2, frameworkRemoved(_));

  EXPECT_CALL(this->allocator2, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator2, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  MockFilter filter;
  process::filter(&filter);

  trigger shutdownMessageTrigger;
  EXPECT_MESSAGE(filter, Eq(ShutdownMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&shutdownMessageTrigger),
			  Return(true)));

  EXPECT_MESSAGE(filter, Eq(ReregisterSlaveMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  Files files;
  Master m(&this->allocator1, &files);
  PID<Master> master1 = process::spawn(&m);

  string zk = "zk://" + this->server->connectString() + "/znode";
  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master1, true, true);
  ASSERT_SOME(detector);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownTrigger;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownTrigger));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);
  Resources resources = Resources::parse("cpus:2;mem:1024");
  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  Try<MasterDetector*> slaveDetector =
    MasterDetector::create(zk, slave, false, true);
  ASSERT_SOME(slaveDetector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO,zk);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  vector<Offer> offers, offers2;
  trigger resourceOffersTrigger, resourceOffersTrigger2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    LaunchTasks(1, 1, 512),
                    Trigger(&resourceOffersTrigger)))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers2),
			  Trigger(&resourceOffersTrigger2)));

  TaskStatus status;
  trigger statusUpdateTrigger;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
		    Trigger(&statusUpdateTrigger)));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  WAIT_UNTIL(resourceOffersTrigger);

  EXPECT_THAT(offers, OfferEq(2, 1024));

  Resources launchedResources = Resources::parse("cpus:1;mem:512");
  trigger resourcesChangedTrigger;
  EXPECT_CALL(isolationModule,
              resourcesChanged(_, _, Resources(launchedResources)))
    .WillOnce(Trigger(&resourcesChangedTrigger));

  WAIT_UNTIL(statusUpdateTrigger);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedTrigger);

  process::terminate(master1);
  process::wait(master1);
  MasterDetector::destroy(detector.get());

  WAIT_UNTIL(shutdownMessageTrigger);

  Files files2;
  Master m2(&(this->allocator2), &files2);
  PID<Master> master2 = process::spawn(m2);

  Try<MasterDetector*> detector2 =
    MasterDetector::create(zk, master2, true, true);
  ASSERT_SOME(detector2);

  WAIT_UNTIL(frameworkAddedTrigger);

  resourceOffersTrigger2.value = false;

  // We kill the filter so that ReregisterSlaveMessages can get
  // to the master now that the framework has been added, ensuring
  // that the slave reregisters after the framework.
  process::filter(NULL);

  WAIT_UNTIL(resourceOffersTrigger2);

  EXPECT_THAT(offers2, OfferEq(1, 512));

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownTrigger); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);
  MasterDetector::destroy(slaveDetector.get());

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master2);
  process::wait(master2);
  MasterDetector::destroy(detector2.get());
}


TYPED_TEST(AllocatorZooKeeperTest, SlaveReregisterFirst)
{
  EXPECT_CALL(this->allocator2, initialize(_, _));

  EXPECT_CALL(this->allocator2, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator2, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator2, frameworkRemoved(_));

  trigger slaveAddedTrigger;
  EXPECT_CALL(this->allocator2, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&this->allocator2),
		    Trigger(&slaveAddedTrigger)));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator2, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  MockFilter filter;
  process::filter(&filter);

  trigger shutdownMessageTrigger;
  EXPECT_MESSAGE(filter, Eq(ShutdownMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&shutdownMessageTrigger),
			  Return(true)));

  EXPECT_MESSAGE(filter, Eq(ReregisterFrameworkMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  Files files;
  Master m(&this->allocator1, &files);
  PID<Master> master1 = process::spawn(&m);

  string zk = "zk://" + this->server->connectString() + "/znode";
  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master1, true, true);
  ASSERT_SOME(detector);

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownTrigger;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownTrigger));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);
  Resources resources = Resources::parse("cpus:2;mem:1024");
  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  Try<MasterDetector*> slaveDetector =
    MasterDetector::create(zk, slave, false, true);
  ASSERT_SOME(slaveDetector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO,zk);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  vector<Offer> offers, offers2;
  trigger resourceOffersTrigger, resourceOffersTrigger2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    LaunchTasks(1, 1, 512),
                    Trigger(&resourceOffersTrigger)))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers2),
			  Trigger(&resourceOffersTrigger2)));

  TaskStatus status;
  trigger statusUpdateTrigger;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
		    Trigger(&statusUpdateTrigger)));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  WAIT_UNTIL(resourceOffersTrigger);

  EXPECT_THAT(offers, OfferEq(2, 1024));

  Resources launchedResources = Resources::parse("cpus:1;mem:512");
  trigger resourcesChangedTrigger;
  EXPECT_CALL(isolationModule,
              resourcesChanged(_, _, Resources(launchedResources)))
    .WillOnce(Trigger(&resourcesChangedTrigger));

  WAIT_UNTIL(statusUpdateTrigger);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedTrigger);

  process::terminate(master1);
  process::wait(master1);
  MasterDetector::destroy(detector.get());

  WAIT_UNTIL(shutdownMessageTrigger);

  Files files2;
  Master m2(&(this->allocator2), &files2);
  PID<Master> master2 = process::spawn(m2);

  Try<MasterDetector*> detector2 =
    MasterDetector::create(zk, master2, true, true);
  ASSERT_SOME(detector2);

  WAIT_UNTIL(slaveAddedTrigger);

  resourceOffersTrigger2.value = false;

  // We kill the filter so that ReregisterFrameworkMessages can get
  // to the master now that the slave has been added, ensuring
  // that the framework reregisters after the slave.
  process::filter(NULL);

  WAIT_UNTIL(resourceOffersTrigger2);

  EXPECT_THAT(offers2, OfferEq(1, 512));

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownTrigger); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);
  MasterDetector::destroy(slaveDetector.get());

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master2);
  process::wait(master2);
  MasterDetector::destroy(detector2.get());
}
