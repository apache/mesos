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

#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include "common/protobuf_utils.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/paths.hpp"
#include "slave/process_based_isolation_module.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "messages/messages.hpp"

#include "tests/filter.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

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


// TODO(vinod): Merge this with the fixture in status updates manager tests.
class SlaveRecoveryTest : public ::testing::Test
{
protected:
  static void SetUpTestCase()
  {
    // Enable checkpointing for the framework.
    frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_checkpoint(true);

    // Enable checkpointing on the slave.
    flags.checkpoint = true;

    // TODO(vinod): Do this for all the tests!
    flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  }

  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    Try<string> workDir = os::mkdtemp();
    CHECK_SOME(workDir) << "Failed to mkdtemp";
    flags.work_dir = workDir.get();

    // Always, drop the unregisterSlaveMessage sent by a slave when
    // its terminated. This will stop the master from removing the slave,
    // which is what we expect to happen in the real world when a slave exits.
    EXPECT_MESSAGE(Eq(UnregisterSlaveMessage().GetTypeName()), _, _)
      .WillRepeatedly(Return(true));

    a = new Allocator(&allocator);
    m = new Master(a, &files);
    master = process::spawn(m);

    startSlave();
  }

  virtual void TearDown()
  {
    stopSlave();

    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;

    os::rmdir(flags.work_dir);
  }

  void startSlave(const Option<string>& recover = None())
  {
    if (recover.isSome()) {
      flags.recover = recover.get(); // Enable recovery.
    }

    isolationModule = new ProcessBasedIsolationModule();
    s = new Slave(flags, true, isolationModule, &files);
    slave = process::spawn(s);

    detector = new BasicMasterDetector(master, slave, true);
  }

  void stopSlave()
  {
    delete detector;

    process::terminate(slave);
    process::wait(slave);
    delete s;

    delete isolationModule;
  }

  HierarchicalDRFAllocatorProcess allocator;
  Allocator *a;
  Master* m;
  ProcessBasedIsolationModule* isolationModule;
  Slave* s;
  Files files;
  BasicMasterDetector* detector;
  MockScheduler sched;
  TaskStatus status;
  PID<Master> master;
  PID<Slave> slave;
  static FrameworkInfo frameworkInfo;
  static flags::Flags<logging::Flags, slave::Flags> flags;
};

// Initialize static members here.
FrameworkInfo SlaveRecoveryTest::frameworkInfo;
flags::Flags<logging::Flags, slave::Flags> SlaveRecoveryTest::flags;


// Enable checkpointing on the slave and ensure SlaveState::recover works.
TEST_F(SlaveRecoveryTest, RecoverSlaveState)
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
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), Eq(master), _)
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

  MesosSchedulerDriver driver(&sched, frameworkInfo, master);

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

  // Capture the ack.
  WAIT_UNTIL(statusUpdateAckMsg);
  StatusUpdateAcknowledgementMessage ack;
  ack.ParseFromString(message4.body);

  sleep(1); // Wait for the ACK to be checkpointed.

  // Recover the state.
  Result<state::SlaveState> recover =
    state::recover(paths::getMetaRootDir(flags.work_dir), true);

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
                .acks.contains(ack.uuid()));

  driver.stop();
  driver.join();
}


// A slave is started with checkpointing enabled (recovery disabled).
// The slave is killed before the ACK for a status update is received.
// The slave is then restarted with recovery enabled.
// Ensure that SUM is properly recovered and re-sends the un-acked update.
TEST_F(SlaveRecoveryTest, RecoverStatusUpdateManager)
{
  // Message expectations.
  trigger statusUpdateAckMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateAcknowledgementMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&statusUpdateAckMsg),
                    Return(true))) // Drop the first ACK message.
    .WillRepeatedly(Return(false));

  // Scheduler expectations.
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
    .WillOnce(Return())
    .WillOnce(DoAll(SaveArg<1>(&status), // This is the update after recovery.
                    Trigger(&statusUpdateCall)));

  MesosSchedulerDriver driver(&sched, frameworkInfo, master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());

  TaskInfo task = createTask(offers[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task); // Long-running task.
  driver.launchTasks(offers[0].id(), tasks);

  // Capture the ack.
  WAIT_UNTIL(statusUpdateAckMsg);

  stopSlave();

  // Restart the slave with recovery enabled.
  startSlave(Option<string>::some("reconnect"));

  WAIT_UNTIL(statusUpdateCall);

  ASSERT_EQ(TASK_RUNNING, status.state());

  driver.stop();
  driver.join();
}

