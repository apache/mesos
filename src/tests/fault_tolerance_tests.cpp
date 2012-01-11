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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/master.hpp"
#include "master/simple_allocator.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;
using mesos::internal::master::SimpleAllocator;

using mesos::internal::slave::ProcessBasedIsolationModule;
using mesos::internal::slave::Slave;
using mesos::internal::slave::STATUS_UPDATE_RETRY_INTERVAL_SECONDS;

using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AnyOf;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;
using testing::SaveArgPointee;


TEST(FaultToleranceTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_EQ(1, offers.size());

  trigger offerRescindedCall, slaveLostCall;

  EXPECT_CALL(sched, offerRescinded(&driver, offers[0].id()))
    .WillOnce(Trigger(&offerRescindedCall));

  EXPECT_CALL(sched, slaveLost(&driver, offers[0].slave_id()))
    .WillOnce(Trigger(&slaveLostCall));

  process::post(slave, process::TERMINATE);

  WAIT_UNTIL(offerRescindedCall);
  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(FaultToleranceTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  process::Clock::pause();

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger slaveLostCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(Trigger(&slaveLostCall));

  EXPECT_MSG(filter, Eq("PONG"), _, _)
    .WillRepeatedly(Return(true));

  driver.start();

  double secs = master::SLAVE_PONG_TIMEOUT * master::MAX_SLAVE_TIMEOUTS;

  process::Clock::advance(secs);

  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  local::shutdown();

  process::filter(NULL);

  process::Clock::resume();
}


TEST(FaultToleranceTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, "", DEFAULT_EXECUTOR_INFO, master);

  FrameworkID frameworkId;

  trigger sched1RegisteredCall;

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&frameworkId), Trigger(&sched1RegisteredCall)));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1RegisteredCall);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, "", DEFAULT_EXECUTOR_INFO,
                               master, frameworkId);

  trigger sched2RegisteredCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  local::shutdown();
}


TEST(FaultToleranceTest, FrameworkReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Drop the registered message, in the first attempt, but allow subsequent
  // tries.
  EXPECT_MSG(filter, Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(Return(true))
    .WillRepeatedly(Return(false));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall); // Ensures registered message is received.

  driver.stop();
  driver.join();

  local::shutdown();

  process::filter(NULL);
}


TEST(FaultToleranceTest, FrameworkReregister)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  process::Message message;
  trigger schedReregisteredMsg;

  EXPECT_MSG(filter, Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgPointee<0>(&message),
                    Return(false)));

  EXPECT_MSG(filter, Eq(FrameworkReregisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&schedReregisteredMsg),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall); // Ensures registered message is received.

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master);

  process::post(message.to, newMasterDetectedMsg);

  WAIT_UNTIL(schedReregisteredMsg);

  driver.stop();
  driver.join();

  local::shutdown();

  process::filter(NULL);
}

// TOOD(vinod): Disabling this test for now because
// of the following race condition breaking this test:
// We do a driver.launchTasks() after post(noMasterDetected)
// but since dispatch (which is used by launchTasks()) uses
// a different queue than post, it might so happen that the latter
// message is dequeued before the former, thus breaking the test.
TEST(FaultToleranceTest, DISABLED_TaskLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  EXPECT_CALL(exec, init(_, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);
  vector<Offer> offers;
  trigger statusUpdateCall, resourceOffersCall;
  TaskStatus status;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  process::Message message;

  EXPECT_MSG(filter, Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgPointee<0>(&message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // Simulate a spurious noMasterDetected event at the scheduler.
  NoMasterDetectedMessage noMasterDetectedMsg;
  process::post(message.to, noMasterDetectedMsg);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(status.state(), TASK_LOST);

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);

  process::filter(NULL);
}


TEST(FaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  process::Clock::pause();

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, "", DEFAULT_EXECUTOR_INFO, master);

  FrameworkID frameworkId;
  vector<Offer> offers;

  trigger resourceOffersCall, statusUpdateMsg;

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  EXPECT_MSG(filter, Eq(StatusUpdateMessage().GetTypeName()), _,
             Not(AnyOf(Eq(master), Eq(slave))))
    .WillOnce(DoAll(Trigger(&statusUpdateMsg), Return(true)))
    .RetiresOnSaturation();

  driver1.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateMsg);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers, at which point advance time enough for the reliable
  // timeout to kick in and another status update message is sent.

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, "", DEFAULT_EXECUTOR_INFO,
                               master, frameworkId);

  trigger registeredCall, statusUpdateCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(Trigger(&statusUpdateCall));

  driver2.start();

  WAIT_UNTIL(registeredCall);

  process::Clock::advance(STATUS_UPDATE_RETRY_INTERVAL_SECONDS);

  WAIT_UNTIL(statusUpdateCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);

  process::filter(NULL);

  process::Clock::resume();
}


TEST(FaultToleranceTest, SchedulerFailoverFrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched1;

  MesosSchedulerDriver driver1(&sched1, "", DEFAULT_EXECUTOR_INFO, master);

  FrameworkID frameworkId;

  vector<Offer> offers;
  TaskStatus status;
  trigger sched1ResourceOfferCall, sched1StatusUpdateCall;

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&sched1StatusUpdateCall)));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(sched1StatusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, "", DEFAULT_EXECUTOR_INFO,
                               master, frameworkId);

  trigger sched2RegisteredCall, sched2FrameworkMessageCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, frameworkMessage(&driver2, _, _, _))
    .WillOnce(Trigger(&sched2FrameworkMessageCall));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  execDriver->sendFrameworkMessage("");

  WAIT_UNTIL(sched2FrameworkMessageCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(FaultToleranceTest, SlaveReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall))
    .WillRepeatedly(Return());

  trigger slaveRegisterMsg;

  // Drop the first registered message, but allow subsequent messages.
  EXPECT_MSG(filter, Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(Return(true))
    .WillRepeatedly(DoAll(Trigger(&slaveRegisterMsg), Return(false)));

  driver.start();

  WAIT_UNTIL(slaveRegisterMsg);

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);

  process::filter(NULL);
}


TEST(FaultToleranceTest, SlaveReregister)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall))
    .WillRepeatedly(Return());

  trigger slaveReRegisterMsg;

  EXPECT_MSG(filter, Eq(SlaveReregisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&slaveReRegisterMsg), Return(false)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the slave.

  NewMasterDetectedMessage message;
  message.set_pid(master);

  process::post(slave, message);

  WAIT_UNTIL(slaveReRegisterMsg);

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);

  process::filter(NULL);
}
