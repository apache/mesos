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

#include <unistd.h>

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

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "tests/cluster.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Isolator;
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
using testing::DoAll;
using testing::Eq;
using testing::Return;


class MasterTest : public MesosClusterTest {};


TEST_F(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Try<PID<Slave> > slave = cluster.slaves.start(&isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> resourcesChanged;
  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(offers.get()[0].resources())))
    .WillOnce(FutureSatisfy(&resourcesChanged));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_UNTIL(resourcesChanged);

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(shutdown); // Ensures MockExecutor can be deallocated.

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  slave::Flags flags = cluster.slaves.flags;
  flags.executor_shutdown_grace_period = Seconds(0);
  Try<PID<Slave> > slave = cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> resourcesChanged;
  EXPECT_CALL(isolator,
              resourcesChanged(_, _, Resources(offers.get()[0].resources())))
    .WillOnce(FutureSatisfy(&resourcesChanged));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_UNTIL(resourcesChanged);

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(shutdown); // Ensures MockExecutor can be deallocated.

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(MasterTest, KillTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  Try<PID<Slave> > slave = cluster.slaves.start(DEFAULT_EXECUTOR_ID, &exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(shutdown); // To ensure can deallocate MockExecutor.

  cluster.shutdown();
}


TEST_F(MasterTest, StatusUpdateAck)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  Try<PID<Slave> > slave = cluster.slaves.start(DEFAULT_EXECUTOR_ID, &exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, Eq(slave.get()));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Ensure the slave gets a status update ACK.
  AWAIT_UNTIL(acknowledgement);

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(shutdown); // Ensures MockExecutor can be deallocated.

  cluster.shutdown();
}


TEST_F(MasterTest, RecoverResources)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  slave::Flags flags = cluster.slaves.flags;
  flags.resources =
    Option<string>("cpus:2;mem:1024;disk:1024;ports:[1-10, 20-30]");
  Try<PID<Slave> > slave = cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources =
    Resources::parse("cpus:0.3;mem:200;ports:[5-8, 23-25]");
  executorInfo.mutable_resources()->MergeFrom(executorResources);

  TaskID taskId;
  taskId.set_value("1");

  Resources taskResources = offers.get()[0].resources() - executorResources;

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_executor()->MergeFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Scheduler should get an offer for killed task's resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.killTask(taskId);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.reviveOffers(); // Don't wait till the next allocation.

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Now kill the executor, scheduler should get an offer it's resources.
  // TODO(benh): WTF? Why aren't we dispatching?
  isolator.killExecutor(offer.framework_id(), executorInfo.executor_id());

  // TODO(benh): We can't do driver.reviveOffers() because we need to
  // wait for the killed executors resources to get aggregated! We
  // should wait for the allocator to recover the resources first. See
  // the allocator tests for inspiration.

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources slaveResources = Resources::parse(flags.resources.get());
  EXPECT_EQ(slaveResources, offers.get()[0].resources());

  driver.stop();
  driver.join();

  // Terminating the slave might cause the mock executor to get a
  // shutdown since the executor driver "links" the slave.
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(MasterTest, FrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  MockExecutor exec;

  Try<PID<Slave> > slave = cluster.slaves.start(DEFAULT_EXECUTOR_ID, &exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  schedDriver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&status));

  schedDriver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<string> execData;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&execData));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello");

  AWAIT_UNTIL(execData);
  EXPECT_EQ("hello", execData.get());

  Future<string> schedData;
  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(FutureArg<3>(&schedData));

  execDriver.get()->sendFrameworkMessage("world");

  AWAIT_UNTIL(schedData);
  EXPECT_EQ("world", schedData.get());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  schedDriver.stop();
  schedDriver.join();

  AWAIT_UNTIL(shutdown); // To ensure can deallocate MockExecutor.

  cluster.shutdown();
}


TEST_F(MasterTest, MultipleExecutors)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  ExecutorID executorId1;
  executorId1.set_value("executor-1");

  ExecutorID executorId2;
  executorId2.set_value("executor-2");

  MockExecutor exec1;
  MockExecutor exec2;

  map<ExecutorID, Executor*> execs;
  execs[executorId1] = &exec1;
  execs[executorId2] = &exec2;

  TestingIsolator isolator(execs);

  Try<PID<Slave> > slave = cluster.slaves.start(&isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  ASSERT_NE(0u, offers.get().size());

  ExecutorInfo executor1; // Bug in gcc 4.1.*, must assign on next line.
  executor1 = CREATE_EXECUTOR_INFO(executorId1, "exit 1");

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task1.mutable_executor()->MergeFrom(executor1);

  ExecutorInfo executor2; // Bug in gcc 4.1.*, must assign on next line.
  executor2 = CREATE_EXECUTOR_INFO(executorId2, "exit 1");

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task2.mutable_executor()->MergeFrom(executor2);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec1Task;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec1Task)));

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec2Task;
  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec2Task)));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(exec1Task);
  EXPECT_EQ(task1.task_id(), exec1Task.get().task_id());

  AWAIT_UNTIL(exec2Task);
  EXPECT_EQ(task2.task_id(), exec2Task.get().task_id());

  AWAIT_UNTIL(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_UNTIL(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  Future<Nothing> shutdown1;
  EXPECT_CALL(exec1, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown1));

  Future<Nothing> shutdown2;
  EXPECT_CALL(exec2, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown2));

  driver.stop();
  driver.join();

  AWAIT_UNTIL(shutdown1); // To ensure can deallocate MockExecutor.
  AWAIT_UNTIL(shutdown2); // To ensure can deallocate MockExecutor.

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(MasterTest, ShutdownUnregisteredExecutor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  ProcessIsolator isolator;

  Try<PID<Slave> > slave = cluster.slaves.start(&isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_UNTIL(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_value("sleep 10");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Drop the registration message from the executor to the slave.
  Future<process::Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_UNTIL(registerExecutor);

  Clock::pause();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure that the slave times out and kills the executor.
  Future<Nothing> killExecutor =
    FUTURE_DISPATCH(_, &Isolator::killExecutor);

  Clock::advance(cluster.slaves.flags.executor_registration_timeout.secs());

  AWAIT_UNTIL(killExecutor);

  Clock::settle(); // Wait for ProcessIsolator::killExecutor to complete.

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(1.0);
    Clock::settle();
  }

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  Clock::resume();

  driver.stop();
  driver.join();

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(MasterTest, MasterInfo)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = cluster.slaves.start();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_UNTIL(masterInfo);
  EXPECT_EQ(master.get().port, masterInfo.get().port());
  EXPECT_EQ(master.get().ip, masterInfo.get().ip());

  driver.stop();
  driver.join();

  cluster.shutdown();
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = cluster.slaves.start();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_UNTIL(message);

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master.get());

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureArg<1>(&masterInfo));

  process::post(message.get().to, newMasterDetectedMsg);

  AWAIT_UNTIL(masterInfo);
  EXPECT_EQ(master.get().port, masterInfo.get().port());
  EXPECT_EQ(master.get().ip, masterInfo.get().ip());

  driver.stop();
  driver.join();

  cluster.shutdown();
}


class WhitelistTest : public MasterTest
{
protected:
  WhitelistTest()
    : path("whitelist.txt")
  {}

  virtual ~WhitelistTest()
  {
    os::rm(path);
  }
  const string path;
};


TEST_F(WhitelistTest, WhitelistSlave)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // Add some hosts to the white list.
  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);
  string hosts = hostname.get() + "\n" + "dummy-slave";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags flags = cluster.masters.flags;
  flags.whitelist = "file://" + path;
  Try<PID<Master> > master = cluster.masters.start(flags);
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = cluster.slaves.start();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_UNTIL(offers); // Implies the slave has registered.

  driver.stop();
  driver.join();

  cluster.shutdown();
}


TEST_F(MasterTest, MasterLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = cluster.slaves.start();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_UNTIL(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious noMasterDetected event at the scheduler.
  process::post(message.get().to, NoMasterDetectedMessage());

  AWAIT_UNTIL(disconnected);

  driver.stop();
  driver.join();

  cluster.shutdown();
}
