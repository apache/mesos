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

#include <gtest/gtest.h>

#include <string>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/dispatch.hpp>
#include <process/gmock.hpp>

#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "common/process_utils.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources.hpp"

#include "detector/detector.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#ifdef __linux__
#include "slave/cgroups_isolator.hpp"
#endif
#include "slave/paths.hpp"
#include "slave/process_isolator.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "messages/messages.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;
using namespace mesos::internal::tests;
using namespace mesos::internal::utils::process;

using namespace process;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

#ifdef __linux__
using mesos::internal::slave::CgroupsIsolator;
#endif
using mesos::internal::slave::ProcessIsolator;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;


class SlaveStateTest : public ::testing::Test
{
public:
  SlaveStateTest()
  {
    Try<string> path = os::mkdtemp();
    CHECK_SOME(path) << "Failed to mkdtemp";
    rootDir = path.get();
  }

  virtual ~SlaveStateTest()
  {
     os::rmdir(rootDir);
  }

protected:
  string rootDir;
};


TEST_F(SlaveStateTest, CheckpointProtobuf)
{
  // Checkpoint slave id.
  SlaveID expected;
  expected.set_value("slave1");

  const string& path = path::join(rootDir, "slave.id");
  state::checkpoint(path, expected);

  const Result<SlaveID>& actual = ::protobuf::read<SlaveID>(path);
  ASSERT_SOME(actual);

  ASSERT_SOME_EQ(expected, actual);
}


TEST_F(SlaveStateTest, CheckpointString)
{
  // Checkpoint a test string.
  const string expected = "test";
  const string path = path::join(rootDir, "test-path");
  state::checkpoint(path, expected);

  ASSERT_SOME_EQ(expected, os::read(path));
}


template <typename T>
class SlaveRecoveryTest : public IsolatorTest<T>
{
public:
  static void SetUpTestCase()
  {
    IsolatorTest<T>::SetUpTestCase();
  }

  virtual void SetUp()
  {
    IsolatorTest<T>::SetUp();

    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    a = new Allocator(&allocator);
    m = new Master(a, &files);
    master = process::spawn(m);

    // Reset recovery slaveFlags.
    this->slaveFlags.checkpoint = true;
    this->slaveFlags.recover = "reconnect";
    this->slaveFlags.safe = false;

    startSlave();
  }

  virtual void TearDown()
  {
    stopSlave(true);

    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;

    IsolatorTest<T>::TearDown();
  }

protected:
  void startSlave()
  {
    isolator = new T();
    s = new Slave(this->slaveFlags, true, isolator, &files);
    slave = process::spawn(s);
    detector = new BasicMasterDetector(master, slave, true);

    running = true;
  }

  void stopSlave(bool shutdown = false)
  {
    if (!running) {
      return;
    }

    delete detector;

    if (shutdown) {
      process::dispatch(slave, &Slave::shutdown);
    } else {
      process::terminate(slave);
    }
    process::wait(slave);
    delete s;
    delete isolator;

    running = false;
  }

  HierarchicalDRFAllocatorProcess allocator;
  Allocator *a;
  Master* m;
  Isolator* isolator;
  Slave* s;
  Files files;
  BasicMasterDetector* detector;
  PID<Master> master;
  PID<Slave> slave;
  bool running; // Is the slave running?
};


#ifdef __linux__
typedef ::testing::Types<ProcessIsolator, CgroupsIsolator> IsolatorTypes;
#else
typedef ::testing::Types<ProcessIsolator> IsolatorTypes;
#endif


TYPED_TEST_CASE(SlaveRecoveryTest, IsolatorTypes);


// Enable checkpointing on the slave and ensure recovery works.
TYPED_TEST(SlaveRecoveryTest, RecoverSlaveState)
{
  // Message expectations.
  process::Message message;
  trigger registerFrameworkMsg;
  EXPECT_MESSAGE(Eq(RegisterFrameworkMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&registerFrameworkMsg),
        Return(false)));

  process::Message message2;
  trigger registerExecutorMsg;
  EXPECT_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message2),
        Trigger(&registerExecutorMsg),
        Return(false)));

  process::Message message3;
  trigger statusUpdateMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), Eq(this->master), _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message3),
        Trigger(&statusUpdateMsg),
        Return(false)));

  process::Message message4;
  trigger statusUpdateAckMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateAcknowledgementMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message4),
        Trigger(&statusUpdateAckMsg),
        Return(false)));

  // Scheduler expectations.
  MockScheduler sched;
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  // Capture the framework pid.
  WAIT_UNTIL(registerFrameworkMsg);
  UPID frameworkPid = message.from;

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  SlaveID slaveId = offers[0].slave_id();

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Capture the executor pids.
  WAIT_UNTIL(registerExecutorMsg);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(message2.body);

  ExecutorID executorId = registerExecutor.executor_id();
  UPID libprocessPid = message2.from;

  // Capture the update.
  WAIT_UNTIL(statusUpdateMsg);
  StatusUpdateMessage update;
  update.ParseFromString(message3.body);

  EXPECT_EQ(TASK_RUNNING, update.update().status().state());

  // Capture the ACK.
  WAIT_UNTIL(statusUpdateAckMsg);
  StatusUpdateAcknowledgementMessage ack;
  ack.ParseFromString(message4.body);

  sleep(1); // Wait for the ACK to be checkpointed.

  // Recover the state.
  Result<state::SlaveState> recover =
    state::recover(paths::getMetaRootDir(this->slaveFlags.work_dir), true);

  ASSERT_SOME(recover);

  state::SlaveState state = recover.get();

  // Check framework id and pid.
  ASSERT_TRUE(state.frameworks.contains(frameworkId));
  ASSERT_SOME_EQ(frameworkPid, state.frameworks[frameworkId].pid);

  ASSERT_TRUE(state.frameworks[frameworkId].executors.contains(executorId));

  // Check executor id and pids.
  const Option<UUID>& uuid=
      state.frameworks[frameworkId].executors[executorId].latest;
  ASSERT_SOME(uuid);

  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs.contains(uuid.get()));

  ASSERT_SOME_EQ(
      libprocessPid,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[uuid.get()]
        .libprocessPid);


  // Check task id and info.
  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs[uuid.get()]
                .tasks.contains(task.task_id()));

  const Task& t = mesos::internal::protobuf::createTask(
      task, TASK_STAGING, executorId, frameworkId);

  ASSERT_SOME_EQ(
      t,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[uuid.get()]
        .tasks[task.task_id()]
        .info);

  // Check status update and ack.
  ASSERT_EQ(
      1U,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[uuid.get()]
        .tasks[task.task_id()]
        .updates.size());

  ASSERT_EQ(
      update.update().uuid(),
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[uuid.get()]
        .tasks[task.task_id()]
        .updates.front().uuid());

  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs[uuid.get()]
                .tasks[task.task_id()]
                .acks.contains(UUID::fromBytes(ack.uuid())));

  // Shut down the executor.
  process::post(libprocessPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is killed before the update reaches the scheduler.
// When the slave comes back up it resends the unacknowledged update.
TYPED_TEST(SlaveRecoveryTest, RecoverStatusUpdateManager)
{
  // Message expectations.
  process::Message message;
  trigger registerExecutorMsg;
  EXPECT_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&registerExecutorMsg),
        Return(false)));

  trigger statusUpdateMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), _, _)
  .WillOnce(DoAll(Trigger(&statusUpdateMsg),
                  Return(true))) // Drop the first update from the executor.
  .WillRepeatedly(Return(false));

  trigger updateFrameworkMsg;
  EXPECT_MESSAGE(Eq(UpdateFrameworkMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&updateFrameworkMsg),
                    Return(false)));

  // Scheduler expectations.
  MockScheduler sched;
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Capture the executor pid.
  WAIT_UNTIL(registerExecutorMsg);
  UPID executorPid = message.from;

  // Wait for the update.
  WAIT_UNTIL(statusUpdateMsg);

  this->stopSlave();

  // Restart the slave.
  this->startSlave();

  // Wait for updated framework pid.
  WAIT_UNTIL(updateFrameworkMsg);

  WAIT_UNTIL(statusUpdateCall);

  ASSERT_EQ(TASK_RUNNING, status.state());

  // Shut down the executor.
  process::post(executorPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up with recovery=reconnect, make
// sure the executor re-registers and the slave properly sends the update.
TYPED_TEST(SlaveRecoveryTest, ReconnectExecutor)
{
  // Message expectations.
  trigger statusUpdateMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), _, _)
  .WillOnce(DoAll(Trigger(&statusUpdateMsg),
                  Return(true))) // Drop the first update from the executor.
  .WillRepeatedly(Return(false));

  process::Message message;
  trigger reregisterExecutorMessage;
  EXPECT_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _)
  .WillOnce(DoAll(
      SaveArgField<0>(&process::MessageEvent::message, &message),
      Trigger(&reregisterExecutorMessage),
      Return(false)));

  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Stop the slave before the status update is received.
  WAIT_UNTIL(statusUpdateMsg);
  this->stopSlave();

  // Restart the slave.
  this->startSlave();

  // Ensure the executor re-registers.
  WAIT_UNTIL(reregisterExecutorMessage);
  UPID executorPid = message.from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(message.body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  WAIT_UNTIL(statusUpdateCall);
  ASSERT_EQ(TASK_RUNNING, status.state());

  // Shut down the executor.
  process::post(executorPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is stopped before the (command) executor is registered.
// When it comes back up with recovery=reconnect, make sure the task is
// properly transitioned to FAILED.
TYPED_TEST(SlaveRecoveryTest, RecoverUnregisteredExecutor)
{
  // Message Expectations.
  process::Message message;
  trigger registerExecutorMsg;
  EXPECT_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&registerExecutorMsg),
        Return(true))); // Drop the executor registration message.

  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Stop the slave before the executor is registered.
  WAIT_UNTIL(registerExecutorMsg);
  UPID executorPid = message.from;
  this->stopSlave();

  // Restart the slave.
  this->startSlave();

  // Scheduler should receive the TASK_FAILED update.
  WAIT_UNTIL(statusUpdateCall);
  ASSERT_EQ(TASK_FAILED, status.state());

  // Shut down the executor.
  process::post(executorPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// The command executor terminates when the slave is down.
// When it comes back up with recovery=reconnect, make
// sure the task is properly transitioned to FAILED.
TYPED_TEST(SlaveRecoveryTest, RecoverTerminatedExecutor)
{
  // Message Expectations.
  process::Message message;
  trigger registerExecutorMsg;
  EXPECT_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&registerExecutorMsg),
        Return(false)));

  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall1, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(Trigger(&statusUpdateCall1))
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall2)));

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Capture the executor pid.
  WAIT_UNTIL(registerExecutorMsg);
  UPID executorPid = message.from;

  // Wait for TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateCall1);

  sleep(1); // Give enough time for the ACK to be checkpointed.

  this->stopSlave();

  // Now shut down the executor, when the slave is down.
  process::post(executorPid, ShutdownExecutorMessage());

  // Restart the slave.
  this->startSlave();

  // Scheduler should receive the TASK_FAILED update.
  WAIT_UNTIL(statusUpdateCall2);
  ASSERT_EQ(TASK_FAILED, status.state());

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// Slave is restarted in recovery=cleanup mode. It kills the command
// executor, and transitions the task to FAILED.
TYPED_TEST(SlaveRecoveryTest, CleanupExecutor)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall1, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(Trigger(&statusUpdateCall1))
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall2)));

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateCall1);

  sleep(1); // Give enough time for the ACK to be checkpointed.

  this->stopSlave();

  // Restart the slave in 'cleanup' recovery mode.
  this->slaveFlags.recover = "cleanup";
  this->startSlave();

  // Scheduler should receive the TASK_FAILED update.
  WAIT_UNTIL(statusUpdateCall2);
  ASSERT_EQ(TASK_FAILED, status.state());

  driver.stop();
  driver.join();
}


// This test checks whether a non-checkpointing framework is
// properly removed, when a checkpointing slave is disconnected.
TYPED_TEST(SlaveRecoveryTest, RemoveNonCheckpointingFramework)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall1, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(Trigger(&statusUpdateCall1))
    .WillOnce(DoAll(SaveArg<1>(&status), // Update after slave exited.
                    Trigger(&statusUpdateCall2)));

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateCall1);

  this->stopSlave();

  // Scheduler should receive the TASK_LOST update.
  WAIT_UNTIL(statusUpdateCall2);
  ASSERT_EQ(TASK_LOST, status.state());

  driver.stop();
  driver.join();
}


// This test ensures that no checkpointing happens for a
// framework that has disabled checkpointing.
TYPED_TEST(SlaveRecoveryTest, NonCheckpointingFramework)
{
  // Scheduler expectations.
  MockScheduler sched;
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  TaskStatus status;
  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(Trigger(&statusUpdateCall))
    .WillRepeatedly(Return());

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateCall);

  // Simulate a 'UpdateFrameworkMessage' to ensure framework pid is
  // not being checkpointed.
  process::dispatch(this->slave, &Slave::updateFramework, frameworkId, "");

  sleep(1); // Give some time for the slave to act on the dispatch.

  // Ensure that the framework info is not being checkpointed.
  const string& path = paths::getFrameworkPath(
      paths::getMetaRootDir(this->slaveFlags.work_dir),
      task.slave_id(),
      frameworkId);

  ASSERT_FALSE(os::exists(path));

  driver.stop();
  driver.join();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. This test ensures that a
// restarted slave is able to communicate with all components
// (scheduler, master, executor).
TYPED_TEST(SlaveRecoveryTest, KillTask)
{
  // Message expectations.
  trigger statusUpdateMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), _, _)
  .WillOnce(DoAll(Trigger(&statusUpdateMsg),
                  Return(true))) // Drop the first update from the executor.
  .WillRepeatedly(Return(false));

  trigger reregisterSlaveMsg;
  EXPECT_MESSAGE(Eq(ReregisterSlaveMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&reregisterSlaveMsg),
                    Return(false)));

  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  trigger resourceOffersCall1, resourceOffersCall2;
  vector<Offer> offers1, offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers1),
                    Trigger(&resourceOffersCall1)))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
                    Trigger(&resourceOffersCall2)))
    .WillRepeatedly(Return());

  TaskStatus status1, status2;
  trigger statusUpdateCall1, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status1), // TASK_RUNNING update.
                    Trigger(&statusUpdateCall1)))
    .WillOnce(DoAll(SaveArg<1>(&status2), // TASK_FAILED update.
                    Trigger(&statusUpdateCall2)));

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall1);

  EXPECT_NE(0u, offers1.size());

  TaskInfo task = createTask(offers1[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers1[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateMsg);

  // Restart the slave.
  this->stopSlave();
  this->startSlave();

  // Wait for the slave to re-register.
  WAIT_UNTIL(reregisterSlaveMsg);

  // Wait for retried TASK_RUNNING update.
  WAIT_UNTIL(statusUpdateCall1);
  ASSERT_EQ(TASK_RUNNING, status1.state());

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_FAILED update.
  WAIT_UNTIL(statusUpdateCall2);
  ASSERT_EQ(TASK_FAILED, status2.state());

  // Make sure all slave resources are reoffered.
  WAIT_UNTIL(resourceOffersCall2);
  ASSERT_EQ(
      Resources(offers1[0].resources()), Resources(offers2[0].resources()));

  driver.stop();
  driver.join();
}
