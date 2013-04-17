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


class SlaveStateTest : public TemporaryDirectoryTest {};


TEST_F(SlaveStateTest, CheckpointProtobuf)
{
  // Checkpoint slave id.
  SlaveID expected;
  expected.set_value("slave1");

  const string& file = "slave.id";
  state::checkpoint(file, expected);

  const Result<SlaveID>& actual = ::protobuf::read<SlaveID>(file);
  ASSERT_SOME(actual);

  ASSERT_SOME_EQ(expected, actual);
}


TEST_F(SlaveStateTest, CheckpointString)
{
  // Checkpoint a test string.
  const string expected = "test";
  const string file = "test-file";
  state::checkpoint(file, expected);

  ASSERT_SOME_EQ(expected, os::read(file));
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
  Future<Message> registerFramework =
    FUTURE_MESSAGE(Eq(RegisterFrameworkMessage().GetTypeName()), _, _);

  // Scheduler expectations.
  MockScheduler sched;
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  // Capture the framework pid.
  AWAIT_READY(registerFramework);
  UPID frameworkPid = registerFramework.get().from;

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  SlaveID slaveId = offers.get()[0].slave_id();

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), Eq(this->master), _);

  Future<StatusUpdateAcknowledgementMessage> ack =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  Future<Nothing> _ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Capture the executor pids.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID libprocessPid = registerExecutorMessage.get().from;

  // Capture the update.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update.get().update().status().state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_ack);

  // Recover the state.
  Result<state::SlaveState> recover =
    state::recover(paths::getMetaRootDir(this->slaveFlags.work_dir), true);

  ASSERT_SOME(recover);

  state::SlaveState state = recover.get();

  // Check slave id.
  ASSERT_EQ(slaveId, state.id);

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
      update.get().update().uuid(),
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
                .acks.contains(UUID::fromBytes(ack.get().uuid())));

  // Shut down the executor.
  process::post(libprocessPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is killed before the update reaches the scheduler.
// When the slave comes back up it resends the unacknowledged update.
TYPED_TEST(SlaveRecoveryTest, RecoverStatusUpdateManager)
{
  // Scheduler expectations.
  MockScheduler sched;

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  // Message expectations.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> update =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  // Wait for the status update drop.
  AWAIT_READY(update);

  this->stopSlave();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // Restart the slave.
  this->startSlave();

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

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
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Stop the slave before the status update is received.
  AWAIT_READY(statusUpdate);

  this->stopSlave();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // Restart the slave.
  this->startSlave();

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  // Shut down the executor.
  process::post(executorPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is stopped before the (command) executor is registered.
// When it comes back up with recovery=reconnect, make sure the
// executor is killed and the task is transitioned to FAILED.
TYPED_TEST(SlaveRecoveryTest, RecoverUnregisteredExecutor)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  // Drop the executor registration message.
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Stop the slave before the executor is registered.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  this->stopSlave();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave.
  this->startSlave();

  Clock::pause();

  AWAIT_READY(recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  Clock::resume();

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// The command executor terminates when the slave is down.
// When it comes back up with recovery=reconnect, make
// sure the task is properly transitioned to FAILED.
TYPED_TEST(SlaveRecoveryTest, RecoverTerminatedExecutor)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  this->stopSlave();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Now shut down the executor, when the slave is down.
  process::post(executorPid, ShutdownExecutorMessage());

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave.
  this->startSlave();

  Clock::pause();

  AWAIT_READY(recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

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

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  this->stopSlave();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Restart the slave in 'cleanup' recovery mode.
  this->slaveFlags.recover = "cleanup";
  this->startSlave();

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  Clock::resume();

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

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task

  Future<Nothing> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  AWAIT_READY(update);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  this->stopSlave();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status.get().state());

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

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task

  Future<Nothing> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait for TASK_RUNNING update.
  AWAIT_READY(update);

  Clock::pause();

  Future<Nothing> updateFramework = FUTURE_DISPATCH(_, &Slave::updateFramework);

  // Simulate a 'UpdateFrameworkMessage' to ensure framework pid is
  // not being checkpointed.
  process::dispatch(this->slave, &Slave::updateFramework, frameworkId, "");

  AWAIT_READY(updateFramework);

  Clock::settle(); // Wait for the slave to act on the dispatch.

  // Ensure that the framework info is not being checkpointed.
  const string& path = paths::getFrameworkPath(
      paths::getMetaRootDir(this->slaveFlags.work_dir),
      task.slave_id(),
      frameworkId);

  ASSERT_FALSE(os::exists(path));

  Clock::resume();

  driver.stop();
  driver.join();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. This test ensures that a
// restarted slave is able to communicate with all components
// (scheduler, master, executor).
TYPED_TEST(SlaveRecoveryTest, KillTask)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), tasks);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Restart the slave.
  this->stopSlave();

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  this->startSlave();

  Clock::pause();

  AWAIT_READY(recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  // Wait for the slave to re-register.
  AWAIT_READY(reregisterSlave);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


// When the slave is down we remove the "latest" symlink in the
// executor's run directory, to simulate a situation where the slave
// cannot recover the executor and hence schedules it for gc.
TYPED_TEST(SlaveRecoveryTest, GCExecutor)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task

  // Capture the slave and framework ids.
  SlaveID slaveId = offers.get()[0].slave_id();
  FrameworkID frameworkId = offers.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Capture the executor id and pid.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage.get().from;

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  this->stopSlave();

  // Now shut down the executor, when the slave is down.
  process::post(executorPid, ShutdownExecutorMessage());

  // Remove the symlink "latest" in the executor directory
  // to simulate a non-recoverable executor.
  ASSERT_SOME(os::rm(paths::getExecutorLatestRunPath(
      paths::getMetaRootDir(this->slaveFlags.work_dir),
      slaveId,
      frameworkId,
      executorId)));

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Restart the slave.
  this->startSlave();

  Clock::pause();

  AWAIT_READY(recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  Clock::settle();

  AWAIT_READY(reregisterSlave);

  Clock::advance(this->slaveFlags.gc_delay);

  Clock::settle();

  // Executor's work and meta directories should be gc'ed by now.
  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      this->slaveFlags.work_dir, slaveId, frameworkId, executorId)));

  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      paths::getMetaRootDir(this->slaveFlags.work_dir),
      slaveId,
      frameworkId,
      executorId)));

  Clock::resume();

  driver.stop();
  driver.join();
}


// The slave is asked to shutdown. When it comes back up, it should
// register as a new slave.
TYPED_TEST(SlaveRecoveryTest, ShutdownSlave)
{
  // Scheduler expectations.
  MockScheduler sched;
  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(Return());       // Ignore the offer when slave is shutting down.

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(&sched, frameworkInfo, this->master);

  driver.start();

  AWAIT_READY(offers1);

  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.

  Future<Nothing> statusUpdate1;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&statusUpdate1))
    .WillOnce(Return());  // Ignore TASK_FAILED update.

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), tasks);

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  AWAIT_READY(statusUpdate1); // Wait for TASK_RUNNING update.

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // We shut down the executor here so that a shutting down slave
  // does not spend too much time waiting for the executor to exit.
  process::post(executorPid, ShutdownExecutorMessage());

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (executorTerminated.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(executorTerminated);

  Clock::resume();

  // Shut down the slave.
  this->stopSlave(true);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Now restart the slave.
  this->startSlave();

  // Ensure that the slave registered with a new id.
  AWAIT_UNTIL(offers2);

  EXPECT_NE(0u, offers2.get().size());

  // Ensure the slave id is different.
  ASSERT_NE(
      offers1.get()[0].slave_id().value(), offers2.get()[0].slave_id().value());

  driver.stop();
  driver.join();
}
