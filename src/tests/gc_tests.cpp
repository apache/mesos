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

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/os.hpp>

#include "detector/detector.hpp"

#include "logging/logging.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
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

class GarbageCollectorTest : public ::testing::Test
{
protected:
  static void SetUpTestCase()
  {
    flags.work_dir = "/tmp/mesos-tests";
  }

  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    process::filter(&filter);

    EXPECT_MESSAGE(filter, _, _, _)
      .WillRepeatedly(Return(false));

    a = new TestAllocatorProcess();
    m = new Master(a);
    master = process::spawn(m);

    execs[DEFAULT_EXECUTOR_ID] = &exec;

    EXPECT_CALL(exec, registered(_, _, _, _))
      .WillRepeatedly(Return());

    EXPECT_CALL(exec, launchTask(_, _))
      .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

    EXPECT_CALL(exec, shutdown(_))
      .WillRepeatedly(Return());

    driver = new MesosSchedulerDriver(&sched, DEFAULT_FRAMEWORK_INFO, master);
  }

  virtual void TearDown()
  {
    stopSlave();

    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;

    process::filter(NULL);

    os::rmdir(flags.work_dir);
  }

  void startSlave()
  {
    isolationModule = new TestingIsolationModule(execs);

    s = new Slave(flags, true, isolationModule);
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

  void restartSlave()
  {
    stopSlave();
    startSlave();
  }

  // Launches a task based on the received offer.
  void launchTask(const ExecutorInfo& executorInfo)
  {
    EXPECT_NE(0u, offers.size());

    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value(UUID::random().toString());
    task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
    task.mutable_resources()->MergeFrom(offers[0].resources());
    task.mutable_executor()->MergeFrom(executorInfo);

    tasks.push_back(task);

    driver->launchTasks(offers[0].id(), tasks);

    tasks.clear();
  }

  void launchTask()
  {
    launchTask(DEFAULT_EXECUTOR_INFO);
  }

  TestAllocatorProcess* a;
  Master* m;
  TestingIsolationModule* isolationModule;
  Slave* s;
  BasicMasterDetector* detector;
  MockExecutor exec, exec1;
  map<ExecutorID, Executor*> execs;
  MockScheduler sched;
  MesosSchedulerDriver* driver;
  trigger resourceOffersCall;
  SlaveRegisteredMessage registeredMsg;
  vector<Offer> offers;
  TaskStatus status;
  vector<TaskInfo> tasks;
  MockFilter filter;
  PID<Master> master;
  PID<Slave> slave;
  static flags::Flags<logging::Flags, slave::Flags> flags;
};


// Initialize static members here.
flags::Flags<logging::Flags, slave::Flags> GarbageCollectorTest::flags;


TEST_F(GarbageCollectorTest, Restart)
{
  process::Message message;
  trigger slaveRegisteredMsg1, slaveRegisteredMsg2;
  EXPECT_MESSAGE(filter, Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&slaveRegisteredMsg1),
        Return(false)))
    .WillOnce(DoAll(Trigger(&slaveRegisteredMsg2), Return(false)));

  trigger lostSlaveMsg;
  EXPECT_MESSAGE(filter, Eq(LostSlaveMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&lostSlaveMsg), Return(false)));

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)));

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return()); // Ignore remaining updates (e.g., TASK_LOST).

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(1);

  startSlave();

  WAIT_UNTIL(slaveRegisteredMsg1);

  // Capture the slave id.
  registeredMsg.ParseFromString(message.body);
  SlaveID slaveId = registeredMsg.slave_id();

  driver->start();

  WAIT_UNTIL(resourceOffersCall);

  launchTask();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  // Make sure directory exists. Need to do this AFTER getting a
  // status update for a task because the directory won't get created
  // until the SlaveRegisteredMessage has been received.
  const std::string& slaveDir = flags.work_dir + "/slaves/" + slaveId.value();
  ASSERT_TRUE(os::exists(slaveDir));

  Clock::pause();

  stopSlave();

  WAIT_UNTIL(lostSlaveMsg);

  startSlave();

  // In order to make sure the slave has scheduled some directories to
  // get garbaged collected we need to wait until the slave has been
  // registered. TODO(benh): We really need to wait until the
  // GarbageCollectorProcess has dispatched a message back to itself.
  WAIT_UNTIL(slaveRegisteredMsg2);

  sleep(1);

  Clock::advance(flags.gc_delay.secs());

  Clock::settle();

  // By this time the old slave directory should be cleaned up.
  ASSERT_FALSE(os::exists(slaveDir));

  Clock::resume();

  driver->stop();
  driver->join();
}


TEST_F(GarbageCollectorTest, ExitedExecutor)
{
  trigger exitedExecutorMsg;
  EXPECT_MESSAGE(filter, Eq(ExitedExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&exitedExecutorMsg), Return(false)));

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)));

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillOnce(Return()) // Ignore the TASK_LOST update.
    .WillRepeatedly(Trigger(&statusUpdateCall));

  startSlave();

  driver->start();

  WAIT_UNTIL(resourceOffersCall);

  launchTask();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  const std::string& executorDir =
    isolationModule->directories[DEFAULT_EXECUTOR_ID];

  ASSERT_TRUE(os::exists(executorDir));

  Clock::pause();

  // Kill the executor and inform the slave.
  isolationModule->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);

  process::dispatch(slave, &Slave::executorExited, frameworkId,
                    DEFAULT_EXECUTOR_ID, 0);

  // In order to make sure the slave has scheduled the executor
  // directory to get garbage collected we need to wait until the
  // slave has sent the ExecutorExited message. TODO(benh): We really
  // need to wait until the GarbageCollectorProcess has dispatched a
  // message back to itself.
  WAIT_UNTIL(exitedExecutorMsg);

  sleep(1);

  Clock::advance(flags.gc_delay.secs());

  Clock::settle();

  // First executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));

  Clock::resume();

  driver->stop();
  driver->join();
}
