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
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "common/resources.hpp"

#include "detector/detector.hpp"

#include "logging/logging.hpp"

#include "local/local.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/filter.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
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

class GarbageCollectorTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    a = new Allocator(&allocator);
    files = new Files();
    m = new Master(a, files);
    master = process::spawn(m);

    execs[DEFAULT_EXECUTOR_ID] = &exec;

    Resources resources = Resources::parse(slaveFlags.resources.get());
    Value::Scalar none;
    cpus = resources.get("cpus", none).value();
    mem = resources.get("mem", none).value();
  }

  virtual void TearDown()
  {
    stopSlave();

    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;
    delete files;

    MesosTest::TearDown();
  }

  void startSlave()
  {
    isolator = new TestingIsolator(execs);

    s = new Slave(slaveFlags, true, isolator, files);
    slave = process::spawn(s);

    detector = new BasicMasterDetector(master, slave, true);
  }

  void stopSlave()
  {
    delete detector;

    process::terminate(slave);
    process::wait(slave);
    delete s;

    delete isolator;
  }

  void restartSlave()
  {
    stopSlave();
    startSlave();
  }

  Allocator* a;
  HierarchicalDRFAllocatorProcess allocator;
  Master* m;
  TestingIsolator* isolator;
  Slave* s;
  Files* files;
  BasicMasterDetector* detector;
  MockExecutor exec, exec1;
  map<ExecutorID, Executor*> execs;
  MockScheduler sched;
  SlaveRegisteredMessage registeredMsg;
  TaskStatus status;
  PID<Master> master;
  PID<Slave> slave;
  double cpus;
  double mem;
};


TEST_F(GarbageCollectorTest, Restart)
{
  // Messages expectations.
  process::Message message;
  trigger slaveRegisteredMsg1, slaveRegisteredMsg2;
  EXPECT_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&slaveRegisteredMsg1),
        Return(false)))
    .WillOnce(DoAll(Trigger(&slaveRegisteredMsg2), Return(false)));

  trigger lostSlaveMsg;
  EXPECT_MESSAGE(Eq(LostSlaveMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(Trigger(&lostSlaveMsg), Return(false)));

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Return());

  // Scheduler expectations.
  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return());

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

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  driver.start();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  // Make sure directory exists. Need to do this AFTER getting a
  // status update for a task because the directory won't get created
  // until the SlaveRegisteredMessage has been received.
  const std::string& slaveDir =
    slaveFlags.work_dir + "/slaves/" + slaveId.value();

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

  Clock::advance(slaveFlags.gc_delay.secs());

  Clock::settle();

  // By this time the old slave directory should be cleaned up.
  ASSERT_FALSE(os::exists(slaveDir));

  Clock::resume();

  driver.stop();
  driver.join();
}


TEST_F(GarbageCollectorTest, ExitedExecutor)
{
  // Messages expectations.
  trigger exitedExecutorMsg;
  EXPECT_MESSAGE(Eq(ExitedExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&exitedExecutorMsg), Return(false)));

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Return());

  // Scheduler expectations.
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return());

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillOnce(Return()) // Ignore the TASK_LOST update.
    .WillRepeatedly(Trigger(&statusUpdateCall));

  startSlave();

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  driver.start();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  const std::string& executorDir =
    isolator->directories[DEFAULT_EXECUTOR_ID];

  process::UPID filesUpid("files", process::ip(), process::port());

  ASSERT_TRUE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::OK().status,
      process::http::get(filesUpid, "browse.json", "path=" + executorDir));

  Clock::pause();

  // Kill the executor and inform the slave.
  isolator->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);

  // In order to make sure the slave has scheduled the executor
  // directory to get garbage collected we need to wait until the
  // slave has sent the ExecutorExited message. TODO(benh): We really
  // need to wait until the GarbageCollectorProcess has dispatched a
  // message back to itself.
  WAIT_UNTIL(exitedExecutorMsg);

  sleep(1);

  Clock::advance(slaveFlags.gc_delay.secs());

  Clock::settle();

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::NotFound().status,
      process::http::get(filesUpid, "browse.json", "path=" + executorDir));

  Clock::resume();

  driver.stop();
  driver.join();
}


TEST_F(GarbageCollectorTest, DiskUsage)
{
  // Messages expectations.
  trigger exitedExecutorMsg;
  EXPECT_MESSAGE(Eq(ExitedExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&exitedExecutorMsg), Return(false)));

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Return());

  // Scheduler expectations.
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return());

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillOnce(Return()) // Ignore the TASK_LOST update.
    .WillRepeatedly(Trigger(&statusUpdateCall));

  startSlave();

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  driver.start();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  const std::string& executorDir =
    isolator->directories[DEFAULT_EXECUTOR_ID];

  process::UPID filesUpid("files", process::ip(), process::port());

  ASSERT_TRUE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::OK().status,
      process::http::get(filesUpid, "browse.json", "path=" + executorDir));

  Clock::pause();

  // Kill the executor and inform the slave.
  isolator->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);

  // In order to make sure the slave has scheduled the executor
  // directory to get garbage collected we need to wait until the
  // slave has sent the ExecutorExited message. TODO(benh): We really
  // need to wait until the GarbageCollectorProcess has dispatched a
  // message back to itself.
  WAIT_UNTIL(exitedExecutorMsg);

  // Simulate a disk full message to the slave.
  process::dispatch(slave, &Slave::_checkDiskUsage, Try<double>::some(1));

  // TODO(vinod): As above, we need to wait until GarbageCollectorProcess has
  // dispatched remove message back to itself.
  sleep(1);

  Clock::settle();

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::NotFound().status,
      process::http::get(filesUpid, "browse.json", "path=" + executorDir));

  Clock::resume();

  driver.stop();
  driver.join();
}
