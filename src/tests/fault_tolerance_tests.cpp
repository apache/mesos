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

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

#include "tests/filter.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::ProcessBasedIsolationModule;
using mesos::internal::slave::Slave;
using mesos::internal::slave::STATUS_UPDATE_RETRY_INTERVAL;

using process::Clock;
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


TEST(FaultToleranceTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_EQ(1u, offers.size());

  trigger offerRescindedCall, slaveLostCall;

  EXPECT_CALL(sched, offerRescinded(&driver, offers[0].id()))
    .WillOnce(Trigger(&offerRescindedCall));

  EXPECT_CALL(sched, slaveLost(&driver, offers[0].slave_id()))
    .WillOnce(Trigger(&slaveLostCall));

  process::terminate(slave);

  WAIT_UNTIL(offerRescindedCall);
  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(FaultToleranceTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  uint32_t pings = 0;

  // Set these expectations up before we spawn the slave (in
  // local::launch) so that we don't miss the first PING.
  EXPECT_MESSAGE(Eq("PING"), _, _)
    .WillRepeatedly(DoAll(Increment(&pings),
                          Return(false)));

  EXPECT_MESSAGE(Eq("PONG"), _, _)
    .WillRepeatedly(Return(true));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffersCall, slaveLostCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(Trigger(&slaveLostCall));

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master, waiting for resource offers should accomplish both.
  WAIT_UNTIL(resourceOffersCall);

  // Now advance through the PINGs.
  do {
    uint32_t count = pings;
    Clock::advance(master::SLAVE_PING_TIMEOUT.secs());
    WAIT_UNTIL(pings == count + 1);
  } while (pings < master::MAX_SLAVE_PING_TIMEOUTS);

  Clock::advance(master::SLAVE_PING_TIMEOUT.secs());

  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  local::shutdown();

  Clock::resume();
}


TEST(FaultToleranceTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;

  trigger sched1RegisteredCall;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&frameworkId), Trigger(&sched1RegisteredCall)));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1RegisteredCall);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  trigger sched2RegisteredCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  local::shutdown();
}


TEST(FaultToleranceTest, FrameworkReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  trigger frameworkRegisteredMsg;

  // Drop the first framework registered message, allow subsequent messages.
  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&frameworkRegisteredMsg),
                    Return(true)))
    .WillRepeatedly(Return(false));

  driver.start();

  WAIT_UNTIL(frameworkRegisteredMsg);

  Clock::advance(1.0); // TODO(benh): Pull out constant from SchedulerProcess.

  WAIT_UNTIL(schedRegisteredCall); // Ensures registered message is received.

  driver.stop();
  driver.join();

  local::shutdown();

  Clock::resume();
}


TEST(FaultToleranceTest, FrameworkReregister)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger schedRegisteredCall, schedReregisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(Trigger(&schedReregisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  process::Message message;

  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall); // Ensures registered message is received.

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master);

  process::post(message.to, newMasterDetectedMsg);

  WAIT_UNTIL(schedReregisteredCall);

  driver.stop();
  driver.join();

  local::shutdown();
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

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);
  vector<Offer> offers;
  trigger statusUpdateCall, resourceOffersCall;
  TaskStatus status;

  EXPECT_CALL(sched, registered(&driver, _, _))
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

  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // Simulate a spurious noMasterDetected event at the scheduler.
  NoMasterDetectedMessage noMasterDetectedMsg;
  process::post(message.to, noMasterDetectedMsg);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(status.state(), TASK_LOST);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(FaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;
  vector<Offer> offers;

  trigger resourceOffersCall, statusUpdateMsg;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .Times(1);

  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), _,
             Not(AnyOf(Eq(master), Eq(slave))))
    .WillOnce(DoAll(Trigger(&statusUpdateMsg), Return(true)))
    .RetiresOnSaturation();

  driver1.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver1.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateMsg);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers, at which point advance time enough for the reliable
  // timeout to kick in and another status update message is sent.

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  trigger registeredCall, statusUpdateCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(Trigger(&statusUpdateCall));

  driver2.start();

  WAIT_UNTIL(registeredCall);

  Clock::advance(STATUS_UPDATE_RETRY_INTERVAL.secs());

  WAIT_UNTIL(statusUpdateCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);

  Clock::resume();
}


TEST(FaultToleranceTest, SchedulerFailoverFrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched1;

  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;

  vector<Offer> offers;
  TaskStatus status;
  trigger sched1ResourceOfferCall, sched1StatusUpdateCall;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&sched1StatusUpdateCall)));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver1.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(sched1StatusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  trigger sched2RegisteredCall, sched2FrameworkMessageCall;

  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
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

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(FaultToleranceTest, SchedulerExit)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  trigger statusUpdateMsg;
  process::Message message;

  // Trigger on the second status update received.
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), _, Eq(master))
    .WillOnce(Return(false))
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Trigger(&statusUpdateMsg),Return(false)));

  MockExecutor exec;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;

  vector<Offer> offers;
  TaskStatus status;
  trigger schedResourceOfferCall, schedStatusUpdateCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&schedStatusUpdateCall)));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&schedResourceOfferCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(schedResourceOfferCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(schedStatusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall);

  // Simulate a executorExited message from isolation module to the slave.
  // We need to explicitly send this message because we don't spawn
  // a real executor process in this test.
  process::dispatch(
      slave,
      &Slave::executorTerminated,
      frameworkId,
      DEFAULT_EXECUTOR_ID,
      0,
      false,
      "Killed executor");

  WAIT_UNTIL(statusUpdateMsg);

  StatusUpdateMessage statusUpdate;
  statusUpdate.ParseFromString(message.body);

  EXPECT_EQ(TASK_LOST, statusUpdate.update().status().state());

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(FaultToleranceTest, SlaveReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  trigger slaveRegisteredMsg;

  // Drop the first slave registered message, allow subsequent messages.
  EXPECT_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&slaveRegisteredMsg), Return(true)))
    .WillRepeatedly(Return(false));

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(slaveRegisteredMsg);

  Clock::advance(1.0); // TODO(benh): Pull out constant from Slave.

  WAIT_UNTIL(resourceOffersCall);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);

  Clock::resume();
}


TEST(FaultToleranceTest, SlaveReregister)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TestAllocatorProcess a;
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall))
    .WillRepeatedly(Return());

  trigger slaveReRegisterMsg;

  EXPECT_MESSAGE(Eq(SlaveReregisteredMessage().GetTypeName()), _, _)
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

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}
