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

#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/containerizer/mesos_containerizer.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::AllocatorProcess;

using mesos::internal::slave::GarbageCollectorProcess;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::MesosContainerizerProcess;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::UPID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more master than slave-related are in this file.
// The others are in "slave_tests.cpp".

class MasterTest : public MesosTest {};


TEST_F(MasterTest, TaskRunning)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
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

  Future<Nothing> resourcesUpdated;
  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&resourcesUpdated),
                    Return(update)));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(resourcesUpdated);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.executor_shutdown_grace_period = Seconds(0);

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
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

  Future<Nothing> resourcesUpdated;
  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&resourcesUpdated),
                    Return(update)));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(resourcesUpdated);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, KillTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a killTask for an unknown task results in a
// TASK_LOST when there are no slaves in transitionary states.
TEST_F(MasterTest, KillUnknownTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  driver.killTask(unknownTaskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, KillUnknownTaskSlaveInTransition)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Start a checkpointing slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  // Wait for slave registration.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Nothing> _reregisterSlave =
    DROP_DISPATCH(_, &Master::_reregisterSlave);

  // Stop master and slave.
  Stop(master.get());
  Stop(slave.get());

  frameworkId = Future<FrameworkID>();
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Restart master.
  master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(frameworkId);

  // Restart slave.
  slave = StartSlave(&exec, slaveFlags);

  // Wait for the slave to start reregistration.
  AWAIT_READY(_reregisterSlave);

  // As Master::killTask isn't doing anything, we shouldn't get a status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  // Set expectation that Master receives killTask message.
  Future<KillTaskMessage> killTaskMessage =
    FUTURE_PROTOBUF(KillTaskMessage(), _, master.get());

  // Attempt to kill unknown task while slave is transitioning.
  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  ASSERT_FALSE(unknownTaskId == task.task_id());

  Clock::pause();

  driver.killTask(unknownTaskId);

  AWAIT_READY(killTaskMessage);

  // Wait for all messages to be dispatched and processed completely to satisfy
  // the expectation that we didn't receive a status update.
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, StatusUpdateAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Ensure the slave gets a status update ACK.
  AWAIT_READY(acknowledgement);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, RecoverResources)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(
      "cpus:2;mem:1024;disk:1024;ports:[1-10, 20-30]");

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources =
    Resources::parse("cpus:0.3;mem:200;ports:[5-8, 23-25]").get();
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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Scheduler should get an offer for killed task's resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.reviveOffers(); // Don't wait till the next allocation.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Now kill the executor, scheduler should get an offer it's resources.
  containerizer.destroy(offer.framework_id(), executorInfo.executor_id());

  // TODO(benh): We can't do driver.reviveOffers() because we need to
  // wait for the killed executors resources to get aggregated! We
  // should wait for the allocator to recover the resources first. See
  // the allocator tests for inspiration.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources slaveResources = Resources::parse(flags.resources.get()).get();
  EXPECT_EQ(slaveResources, offers.get()[0].resources());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, FrameworkMessage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  schedDriver.start();

  AWAIT_READY(offers);
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

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<string> execData;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&execData));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello");

  AWAIT_READY(execData);
  EXPECT_EQ("hello", execData.get());

  Future<string> schedData;
  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(FutureArg<3>(&schedData));

  execDriver.get()->sendFrameworkMessage("world");

  AWAIT_READY(schedData);
  EXPECT_EQ("world", schedData.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();

  Shutdown();
}


TEST_F(MasterTest, MultipleExecutors)
{
  Try<PID<Master> > master = StartMaster();
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

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor1);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
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

  AWAIT_READY(exec1Task);
  EXPECT_EQ(task1.task_id(), exec1Task.get().task_id());

  AWAIT_READY(exec2Task);
  EXPECT_EQ(task2.task_id(), exec2Task.get().task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, MasterInfo)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().port, masterInfo.get().port());
  EXPECT_EQ(master.get().ip, masterInfo.get().ip());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave> > slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);
  AWAIT_READY(resourceOffers);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureArg<1>(&masterInfo));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(disconnected);

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().port, masterInfo.get().port());
  EXPECT_EQ(master.get().ip, masterInfo.get().ip());

  // The re-registered framework should get offers.
  AWAIT_READY(resourceOffers2);

  driver.stop();
  driver.join();

  Shutdown();
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
  // Add some hosts to the white list.
  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);

  string hosts = hostname.get() + "\n" + "dummy-slave";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags flags = CreateMasterFlags();
  flags.whitelist = "file://" + path;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.hostname = hostname.get();
  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers); // Implies the slave has registered.

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, MasterLost)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  driver.stop();
  driver.join();

  Shutdown();
}


// Test ensures two offers from same slave can be used for single task.
// This is done by first launching single task which utilize half of the
// available resources. A subsequent offer for the rest of the available
// resources will be sent by master. The first task is killed and an offer
// for the remaining resources will be sent. Which means two offers covering
// all slave resources and a single task should be able to run on these.
TEST_F(MasterTest, LaunchCombinedOfferTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // The CPU granularity is 1.0 which means that we need slaves with at least
  // 2 cpus for a combined offer.
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Get 1st offer and use half of the slave resources to get subsequent offer.
  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks1;
  tasks1.push_back(task1);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), tasks1, filters);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Await 2nd offer.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  Resources resources2(offers2.get()[0].resources());
  EXPECT_EQ(1, resources2.cpus().get());
  EXPECT_EQ(Megabytes(512), resources2.mem().get());

  Future<TaskStatus> status2;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3));

  // Kill 1st task.
  TaskID taskId1 = task1.task_id();
  driver.killTask(taskId1);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2.get().state());

  // Await 3rd offer - 2nd and 3rd offer to same slave are now ready.
  AWAIT_READY(offers3);
  EXPECT_NE(0u, offers3.get().size());
  Resources resources3(offers3.get()[0].resources());
  EXPECT_EQ(1, resources3.cpus().get());
  EXPECT_EQ(Megabytes(512), resources3.mem().get());

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers2.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(fullSlave);
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks2;
  tasks2.push_back(task2);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status3));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers2.get()[0].id());
  combinedOffers.push_back(offers3.get()[0].id());

  driver.launchTasks(combinedOffers, tasks2);

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures offers for launchTasks cannot span multiple slaves.
TEST_F(MasterTest, LaunchAcrossSlavesTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();
  Resources twoSlaves = fullSlave + fullSlave;

  slave::Flags flags = CreateSlaveFlags();

  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave1 = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  // Test that offers cannot span multiple slaves.
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  Try<PID<Slave> > slave2 = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  Resources resources2(offers1.get()[0].resources());
  EXPECT_EQ(2, resources2.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources2.mem().get());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(twoSlaves);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers1.get()[0].id());
  combinedOffers.push_back(offers2.get()[0].id());

  Future<Nothing> resourcesRecovered =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesRecovered);

  driver.launchTasks(combinedOffers, tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(resourcesRecovered);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that an offer cannot appear more than once in offers
// for launchTasks.
TEST_F(MasterTest, LaunchDuplicateOfferTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave> > slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Test that same offers cannot be used more than once.
  // Kill 2nd task and get offer for full slave.
  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources.mem().get());

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers.get()[0].id());
  combinedOffers.push_back(offers.get()[0].id());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(fullSlave);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> resourcesRecovered =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesRecovered);

  driver.launchTasks(combinedOffers, tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(resourcesRecovered);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test runs a task but intercepts the scheduler driver's
// acknowledgement messages to the slave and instead sends them to
// the master. This test is necessary to test that the
// acknowledgement handling in the master is correct, but once the
// driver sends these messages we should remove/update this test!
TEST_F(MasterTest, StatusUpdateAcknowledgementsThroughMaster)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // We need to grab this message to get the scheduler's pid.
  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  schedDriver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&update));

  // Pause the clock to prevent status update retries on the slave.
  Clock::pause();

  // Intercept the status update acknowledgement and send it to the
  // master instead!
  Future<StatusUpdateAcknowledgementMessage> acknowledgementMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(),
                  schedulerPid,
                  slave.get());

  schedDriver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update.get().state());

  AWAIT_READY(acknowledgementMessage);

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdateAcknowledgement);

  // Send the acknowledgement to the master.
  process::post(schedulerPid, master.get(), acknowledgementMessage.get());

  // Ensure the slave receives and properly handles the ACK.
  // Clock::settle() ensures that the slave successfully
  // executes Slave::_statusUpdateAcknowledgement().
  AWAIT_READY(_statusUpdateAcknowledgement);
  Clock::settle();

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();

  Shutdown();
}


TEST_F(MasterTest, MetricsInStatsEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Future<process::http::Response> response =
    process::http::get(master.get(), "stats.json");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, stats.values.count("elected"));
  EXPECT_EQ(1u, stats.values.count("master/elected"));

  EXPECT_EQ(1, stats.values["elected"]);
  EXPECT_EQ(1, stats.values["master/elected"]);

  EXPECT_EQ(1u, stats.values.count("master/slaves_active"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, stats.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));

  EXPECT_EQ(1u, stats.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, stats.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, stats.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, stats.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_framework_to_executor"));

  // Messages from slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, stats.values.count("master/messages_exited_executor"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_framework_to_executor_messages"));

  EXPECT_EQ(1u, stats.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_status_update_acknowledgements"));

  EXPECT_EQ(1u, stats.values.count("master/recovery_slave_removals"));

  EXPECT_EQ(1u, stats.values.count("master/event_queue_size"));

  EXPECT_EQ(1u, stats.values.count("master/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("master/mem_total"));
  EXPECT_EQ(1u, stats.values.count("master/mem_used"));
  EXPECT_EQ(1u, stats.values.count("master/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("master/disk_total"));
  EXPECT_EQ(1u, stats.values.count("master/disk_used"));
  EXPECT_EQ(1u, stats.values.count("master/disk_percent"));

  EXPECT_EQ(1u, stats.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, stats.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, stats.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, stats.values.count("registrar/state_store_ms"));

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// but does not re-register with the master, it is removed from the
// registry and the framework is informed that the slave is lost, and
// the slave is refused re-registration.
TEST_F(MasterTest, RecoveredSlaveDoesNotReregister)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock until the re-registration timeout
  // elapses, and expect the slave / task to be lost!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);

  AWAIT_READY(slaveLost);

  Clock::resume();

  // Step 7: Ensure the slave cannot re-register!
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(shutdownMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a non-strict registry is write-only by
// inducing a slave removal during recovery. After which, we expect
// that the framework is *not* informed, and we expect that the
// slave can re-register successfully.
TEST_F(MasterTest, NonStrictRegistryWriteOnly)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = false;

  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  Clock::resume();

  // Step 7: Now expect the slave to be able to re-register,
  // according to the non-strict semantics.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// and re-registers with the master, it is *not* removed after the
// re-registration timeout elapses.
TEST_F(MasterTest, RecoveredSlaveReregisters)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master> > master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Setup recovery slave flags.
  slaveFlags.checkpoint = true;
  slaveFlags.recover = "reconnect";
  slaveFlags.strict = true;

  Try<PID<Slave> > slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler to ensure the master would notify
  // a framework, were a slave to be lost.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(registered);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef MESOS_HAS_JAVA

class MasterZooKeeperTest : public MesosZooKeeperTest {};

// This test verifies that when the ZooKeeper cluster is lost,
// master, slave & scheduler all get informed.
TEST_F(MasterZooKeeperTest, LostZooKeeperCluster)
{
  ASSERT_SOME(StartMaster());
  ASSERT_SOME(StartSlave());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(url.get()), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);
  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  // Wait for the "registered" messages so that we know the master is
  // detected by everyone.
  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(slaveRegisteredMessage);

  Future<Nothing> schedulerDisconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&schedulerDisconnected));

  // Need to drop these two dispatches because otherwise the master
  // will EXIT.
  Future<Nothing> masterDetected = DROP_DISPATCH(_, &Master::detected);
  Future<Nothing> lostCandidacy = DROP_DISPATCH(_, &Master::lostCandidacy);

  Future<Nothing> slaveDetected = FUTURE_DISPATCH(_, &Slave::detected);

  server->shutdownNetwork();

  Clock::pause();

  while (schedulerDisconnected.isPending() ||
         masterDetected.isPending() ||
         slaveDetected.isPending() ||
         lostCandidacy.isPending()) {
    Clock::advance(MASTER_CONTENDER_ZK_SESSION_TIMEOUT);
    Clock::settle();
  }

  Clock::resume();

  // Master, slave and scheduler all lose the leading master.
  AWAIT_READY(schedulerDisconnected);
  AWAIT_READY(masterDetected);
  AWAIT_READY(lostCandidacy);
  AWAIT_READY(slaveDetected);

  driver.stop();
  driver.join();

  Shutdown();
}

#endif // MESOS_HAS_JAVA
