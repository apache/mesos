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

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;


TEST(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
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

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger resourceOfferCall, statusUpdateCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(MasterTest, KillTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger killTaskCall;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(Trigger(&killTaskCall));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger resourceOfferCall, statusUpdateCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskID taskId;
  taskId.set_value("1");

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.killTask(taskId);

  WAIT_UNTIL(killTaskCall);

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(MasterTest, FrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;
  ExecutorArgs args;
  string execData;

  trigger execFrameworkMessageCall;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver), SaveArg<1>(&args)));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(DoAll(SaveArg<1>(&execData),
                    Trigger(&execFrameworkMessageCall)));

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

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;
  string schedData;

  trigger resourceOfferCall, statusUpdateCall, schedFrameworkMessageCall;

  EXPECT_CALL(sched, getFrameworkName(&schedDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&schedDriver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&schedDriver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&schedDriver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(DoAll(SaveArg<3>(&schedData),
                    Trigger(&schedFrameworkMessageCall)));

  schedDriver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  schedDriver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  string hello = "hello";

  schedDriver.sendFrameworkMessage(offers[0].slave_id(),
				   DEFAULT_EXECUTOR_ID,
				   hello);

  WAIT_UNTIL(execFrameworkMessageCall);

  EXPECT_EQ(hello, execData);

  string reply = "reply";

  execDriver->sendFrameworkMessage(reply);

  WAIT_UNTIL(schedFrameworkMessageCall);

  EXPECT_EQ(reply, schedData);

  schedDriver.stop();
  schedDriver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(MasterTest, MultipleExecutors)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID<Master> master = process::spawn(&m);

  MockExecutor exec1;
  TaskDescription exec1Task;
  trigger exec1LaunchTaskCall;

  EXPECT_CALL(exec1, init(_, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec1Task),
                    Trigger(&exec1LaunchTaskCall),
                    SendStatusUpdate(TASK_RUNNING)));

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  MockExecutor exec2;
  TaskDescription exec2Task;
  trigger exec2LaunchTaskCall;

  EXPECT_CALL(exec2, init(_, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec2Task),
                    Trigger(&exec2LaunchTaskCall),
                    SendStatusUpdate(TASK_RUNNING)));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  ExecutorID executorId1;
  executorId1.set_value("executor-1");

  ExecutorID executorId2;
  executorId2.set_value("executor-2");

  map<ExecutorID, Executor*> execs;
  execs[executorId1] = &exec1;
  execs[executorId2] = &exec2;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status1, status2;

  trigger resourceOfferCall, statusUpdateCall1, statusUpdateCall2;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status1), Trigger(&statusUpdateCall1)))
    .WillOnce(DoAll(SaveArg<1>(&status2), Trigger(&statusUpdateCall2)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_NE(0, offers.size());

  TaskDescription task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task1.mutable_executor()->mutable_executor_id()->MergeFrom(executorId1);
  task1.mutable_executor()->set_uri("noexecutor");

  TaskDescription task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task2.mutable_executor()->mutable_executor_id()->MergeFrom(executorId2);
  task2.mutable_executor()->set_uri("noexecutor");

  vector<TaskDescription> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  driver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall1);

  EXPECT_EQ(TASK_RUNNING, status1.state());

  WAIT_UNTIL(statusUpdateCall2);

  EXPECT_EQ(TASK_RUNNING, status2.state());

  WAIT_UNTIL(exec1LaunchTaskCall);

  EXPECT_EQ(task1.task_id(), exec1Task.task_id());

  WAIT_UNTIL(exec2LaunchTaskCall);

  EXPECT_EQ(task2.task_id(), exec2Task.task_id());

  driver.stop();
  driver.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}
