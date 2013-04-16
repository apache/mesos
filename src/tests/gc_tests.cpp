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
#include <stout/nothing.hpp>
#include <stout/os.hpp>

#include "common/resources.hpp"

#include "detector/detector.hpp"

#include "logging/logging.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

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
using testing::Return;

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
  Clock::advance(Seconds(10));
  Clock::settle();

  ASSERT_FUTURE_WILL_SUCCEED(schedule1);
  ASSERT_FUTURE_WILL_SUCCEED(schedule2);
  ASSERT_TRUE(schedule3.isPending());

  EXPECT_FALSE(os::exists(file1));
  EXPECT_FALSE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));

  // Trigger the GC of file3.
  Clock::advance(Seconds(5));
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
  Clock::advance(Seconds(10));
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


class GarbageCollectorIntegrationTest : public MesosClusterTest {};

TEST_F(GarbageCollectorIntegrationTest, Restart)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec;

  Try<PID<Slave> > slave = cluster.slaves.start(DEFAULT_EXECUTOR_ID, &exec);
  ASSERT_SOME(slave);

  AWAIT_UNTIL(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  Resources resources = Resources::parse(cluster.slaves.flags.resources.get());
  double cpus = resources.get("cpus", Value::Scalar()).value();
  double mem = resources.get("mem", Value::Scalar()).value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Make sure directory exists. Need to do this AFTER getting a
  // status update for a task because the directory won't get created
  // until the task is launched. We get the slave ID from the
  // SlaveRegisteredMessage.
  const std::string& slaveDir = slave::paths::getSlavePath(
      cluster.slaves.flags.work_dir,
      slaveRegisteredMessage.get().slave_id());

  ASSERT_TRUE(os::exists(slaveDir));

  Clock::pause();

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(_, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  cluster.slaves.stop(slave.get());

  AWAIT_UNTIL(shutdown); // Ensures MockExecutor can be deallocated.

  AWAIT_UNTIL(slaveLost);

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  slave = cluster.slaves.start();
  ASSERT_SOME(slave);

  AWAIT_UNTIL(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Clock::advance(cluster.slaves.flags.gc_delay);

  Clock::settle();

  // By this time the old slave directory should be cleaned up.
  ASSERT_FALSE(os::exists(slaveDir));

  Clock::resume();

  driver.stop();
  driver.join();

  cluster.shutdown();
}


TEST_F(GarbageCollectorIntegrationTest, ExitedExecutor)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Resources resources = Resources::parse(cluster.slaves.flags.resources.get());
  double cpus = resources.get("cpus", Value::Scalar()).value();
  double mem = resources.get("mem", Value::Scalar()).value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_UNTIL(frameworkId);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  const std::string& executorDir = isolator.directories[DEFAULT_EXECUTOR_ID];

  ASSERT_TRUE(os::exists(executorDir));

  process::UPID files("files", process::ip(), process::port());
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::OK().status,
      process::http::get(files, "browse.json", "path=" + executorDir));

  Clock::pause();

  // Kiling the executor will cause the slave to schedule its
  // directory to get garbage collected.
  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  // Kill the executor and inform the slave.
  // TODO(benh): WTF? Why aren't we dispatching?
  isolator.killExecutor(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  AWAIT_UNTIL(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Clock::advance(cluster.slaves.flags.gc_delay);

  Clock::settle();

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::NotFound().status,
      process::http::get(files, "browse.json", "path=" + executorDir));

  Clock::resume();

  driver.stop();
  driver.join();

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}


TEST_F(GarbageCollectorIntegrationTest, DiskUsage)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Resources resources = Resources::parse(cluster.slaves.flags.resources.get());
  double cpus = resources.get("cpus", Value::Scalar()).value();
  double mem = resources.get("mem", Value::Scalar()).value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, cpus, mem))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_UNTIL(frameworkId);

  AWAIT_UNTIL(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  const std::string& executorDir = isolator.directories[DEFAULT_EXECUTOR_ID];

  ASSERT_TRUE(os::exists(executorDir));

  process::UPID files("files", process::ip(), process::port());
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::OK().status,
      process::http::get(files, "browse.json", "path=" + executorDir));

  Clock::pause();

  // Kiling the executor will cause the slave to schedule its
  // directory to get garbage collected.
  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  // Kill the executor and inform the slave.
  // TODO(benh): WTF? Why aren't we dispatching?
  isolator.killExecutor(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  AWAIT_UNTIL(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Future<Nothing> _checkDiskUsage =
    FUTURE_DISPATCH(_, &Slave::_checkDiskUsage);

  // Simulate a disk full message to the slave.
  process::dispatch(slave.get(), &Slave::_checkDiskUsage, Try<double>::some(1));

  AWAIT_UNTIL(_checkDiskUsage);

  Clock::settle(); // Wait for Slave::_checkDiskUsage to complete.

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      process::http::NotFound().status,
      process::http::get(files, "browse.json", "path=" + executorDir));

  Clock::resume();

  driver.stop();
  driver.join();

  cluster.shutdown(); // Must shutdown before 'isolator' gets deallocated.
}
