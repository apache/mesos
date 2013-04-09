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
#include <process/gmock.hpp>
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
#include "slave/gc.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::GarbageCollector;
using mesos::internal::slave::GarbageCollectorProcess;
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


class GarbageCollectorTest : public TemporaryDirectoryTest {};


TEST_F(GarbageCollectorTest, Schedule)
{
  GarbageCollector gc;

  // Make some temporary files to gc.
  const string& file1 = "file1";
  const string& file2 = "file2";
  const string& file3 = "file3";

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  ASSERT_TRUE(os::exists(file1));
  ASSERT_TRUE(os::exists(file2));
  ASSERT_TRUE(os::exists(file3));

  Clock::pause();

  Future<Nothing> scheduleDispatch1 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);
  Future<Nothing> scheduleDispatch2 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);
  Future<Nothing> scheduleDispatch3 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Schedule the gc operations.
  Future<Nothing> schedule1 = gc.schedule(Seconds(10), file1);
  Future<Nothing> schedule2 = gc.schedule(Seconds(10), file2);
  Future<Nothing> schedule3 = gc.schedule(Seconds(15), file3);

  // Ensure the dispatches are completed before advancing the clock.
  AWAIT_UNTIL(scheduleDispatch1);
  AWAIT_UNTIL(scheduleDispatch2);
  AWAIT_UNTIL(scheduleDispatch3);
  Clock::settle();

  // Advance the clock to trigger the GC of file1 and file2.
  Clock::advance(Seconds(10).secs());
  Clock::settle();

  ASSERT_FUTURE_WILL_SUCCEED(schedule1);
  ASSERT_FUTURE_WILL_SUCCEED(schedule2);
  ASSERT_TRUE(schedule3.isPending());

  EXPECT_FALSE(os::exists(file1));
  EXPECT_FALSE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));

  // Trigger the GC of file3.
  Clock::advance(Seconds(5).secs());
  Clock::settle();

  ASSERT_FUTURE_WILL_SUCCEED(schedule3);

  EXPECT_FALSE(os::exists(file3));

  Clock::resume();
}


TEST_F(GarbageCollectorTest, Unschedule)
{
  GarbageCollector gc;

  // Attempt to unschedule a file that is not scheduled.
  ASSERT_FUTURE_WILL_EQ(false, gc.unschedule("bogus"));

  // Make some temporary files to gc.
  const string& file1 = "file1";
  const string& file2 = "file2";
  const string& file3 = "file3";

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  ASSERT_TRUE(os::exists(file1));
  ASSERT_TRUE(os::exists(file2));
  ASSERT_TRUE(os::exists(file3));

  Clock::pause();

  // Schedule the gc operations.
  Future<Nothing> schedule1 = gc.schedule(Seconds(10), file1);
  Future<Nothing> schedule2 = gc.schedule(Seconds(10), file2);
  Future<Nothing> schedule3 = gc.schedule(Seconds(10), file3);

  // Unschedule each operation.
  ASSERT_FUTURE_WILL_EQ(true, gc.unschedule(file2));
  ASSERT_FUTURE_WILL_EQ(true, gc.unschedule(file3));
  ASSERT_FUTURE_WILL_EQ(true, gc.unschedule(file1));

  // Advance the clock to ensure nothing was GCed.
  Clock::advance(Seconds(10).secs());
  Clock::settle();

  // The unscheduling will have discarded the GC futures.
  ASSERT_FUTURE_WILL_DISCARD(schedule1);
  ASSERT_FUTURE_WILL_DISCARD(schedule2);
  ASSERT_FUTURE_WILL_DISCARD(schedule3);

  EXPECT_TRUE(os::exists(file1));
  EXPECT_TRUE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));

  Clock::resume();
}


TEST_F(GarbageCollectorTest, Prune)
{
  GarbageCollector gc;

  // Make some temporary files to prune.
  const string& file1 = "file1";
  const string& file2 = "file2";
  const string& file3 = "file3";
  const string& file4 = "file4";

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));
  ASSERT_SOME(os::touch(file4));

  ASSERT_TRUE(os::exists(file1));
  ASSERT_TRUE(os::exists(file2));
  ASSERT_TRUE(os::exists(file3));
  ASSERT_TRUE(os::exists(file4));

  Clock::pause();

  Future<Nothing> schedule1 = gc.schedule(Seconds(10), file1);
  Future<Nothing> schedule2 = gc.schedule(Seconds(10), file2);
  Future<Nothing> schedule3 = gc.schedule(Seconds(15), file3);
  Future<Nothing> schedule4 = gc.schedule(Seconds(15), file4);

  ASSERT_FUTURE_WILL_EQ(true, gc.unschedule(file3));
  ASSERT_FUTURE_WILL_DISCARD(schedule3);

  // Prune file1 and file2.
  gc.prune(Seconds(10));

  ASSERT_FUTURE_WILL_SUCCEED(schedule1);
  ASSERT_FUTURE_WILL_SUCCEED(schedule2);
  ASSERT_TRUE(schedule4.isPending());

  // Both file1 and file2 will have been removed.
  EXPECT_FALSE(os::exists(file1));
  EXPECT_FALSE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));
  EXPECT_TRUE(os::exists(file4));

  // Prune file4.
  gc.prune(Seconds(15));

  ASSERT_FUTURE_WILL_SUCCEED(schedule4);

  EXPECT_FALSE(os::exists(file4));

  Clock::resume();
}


class GarbageCollectorIntegrationTest : public MesosTest
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


TEST_F(GarbageCollectorIntegrationTest, Restart)
{
  // Messages expectations.
  process::Message message;
  trigger slaveRegisteredMsg;
  EXPECT_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&slaveRegisteredMsg),
        Return(false)))
    .WillOnce(Return(false));

  trigger lostSlaveMsg;
  EXPECT_MESSAGE(Eq(LostSlaveMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&lostSlaveMsg),
                    Return(false)));

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

  WAIT_UNTIL(slaveRegisteredMsg);

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

  trigger scheduleDispatch;
  EXPECT_DISPATCH(_, &GarbageCollectorProcess::schedule)
    .WillOnce(DoAll(Trigger(&scheduleDispatch),
                    Return(false)));

  startSlave();

  WAIT_UNTIL(scheduleDispatch);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Clock::advance(slaveFlags.gc_delay.secs());

  Clock::settle();

  // By this time the old slave directory should be cleaned up.
  ASSERT_FALSE(os::exists(slaveDir));

  Clock::resume();

  driver.stop();
  driver.join();
}


TEST_F(GarbageCollectorIntegrationTest, ExitedExecutor)
{
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

  trigger scheduleDispatch;
  EXPECT_DISPATCH(_, &GarbageCollectorProcess::schedule)
    .WillOnce(DoAll(Trigger(&scheduleDispatch),
                    Return(false)));

  // Kill the executor and inform the slave.
  isolator->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);

  WAIT_UNTIL(scheduleDispatch);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

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


TEST_F(GarbageCollectorIntegrationTest, DiskUsage)
{
  // Messages expectations.
  trigger exitedExecutorMsg;
  EXPECT_MESSAGE(Eq(ExitedExecutorMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&exitedExecutorMsg),
                    Return(false)));

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

  trigger scheduleDispatch;
  EXPECT_DISPATCH(_, &GarbageCollectorProcess::schedule)
    .WillOnce(DoAll(Trigger(&scheduleDispatch),
                    Return(false)));

  // Kill the executor and inform the slave.
  isolator->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);

  WAIT_UNTIL(scheduleDispatch);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  trigger _checkDiskUsageDispatch;
  EXPECT_DISPATCH(_, &Slave::_checkDiskUsage)
    .WillOnce(DoAll(Trigger(&_checkDiskUsageDispatch),
                    Return(false)));

  // Simulate a disk full message to the slave.
  process::dispatch(slave, &Slave::_checkDiskUsage, Try<double>::some(1));

  WAIT_UNTIL(_checkDiskUsageDispatch);

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
