// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <list>
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "logging/logging.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/gc_process.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::GarbageCollector;
using mesos::internal::slave::GarbageCollectorProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Timeout;

using std::list;
using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AllOf;
using testing::AtMost;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

class GarbageCollectorTest : public TemporaryDirectoryTest {};

TEST_F(GarbageCollectorTest, Schedule)
{
  GarbageCollector gc("work_dir");

  // Make some temporary files to gc.
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

  Future<Nothing> scheduleDispatch1 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);
  Future<Nothing> scheduleDispatch2 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);
  Future<Nothing> scheduleDispatch3 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);
  Future<Nothing> scheduleDispatch4 =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Schedule the gc operations.
  Future<Nothing> schedule1 = gc.schedule(Seconds(10), file1);
  Future<Nothing> schedule2 = gc.schedule(Seconds(10), file2);
  Future<Nothing> schedule3 = gc.schedule(Seconds(15), file3);
  Future<Nothing> schedule4 = gc.schedule(Seconds(-15), file4);

  // Ensure the dispatches are completed before advancing the clock.
  AWAIT_READY(scheduleDispatch1);
  AWAIT_READY(scheduleDispatch2);
  AWAIT_READY(scheduleDispatch3);
  AWAIT_READY(scheduleDispatch4);
  Clock::settle();

  AWAIT_READY(schedule4);

  JSON::Object metrics = Metrics();

  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_pending"));
  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_succeeded"));
  EXPECT_SOME_EQ(
      3u,
      metrics.at<JSON::Number>("gc/path_removals_pending"));
  EXPECT_SOME_EQ(
      1u,
      metrics.at<JSON::Number>("gc/path_removals_succeeded"));

  // Advance the clock to trigger the GC of file1 and file2.
  Clock::advance(Seconds(10));
  Clock::settle();

  AWAIT_READY(schedule1);
  AWAIT_READY(schedule2);
  ASSERT_TRUE(schedule3.isPending());

  EXPECT_FALSE(os::exists(file1));
  EXPECT_FALSE(os::exists(file2));
  EXPECT_FALSE(os::exists(file4));
  EXPECT_TRUE(os::exists(file3));

  // Trigger the GC of file3.
  Clock::advance(Seconds(5));
  Clock::settle();

  AWAIT_READY(schedule3);

  EXPECT_FALSE(os::exists(file3));

  metrics = Metrics();

  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_pending"));
  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_succeeded"));
  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_failed"));

  EXPECT_SOME_EQ(
      0u,
      metrics.at<JSON::Number>("gc/path_removals_pending"));
  EXPECT_SOME_EQ(
      4u,
      metrics.at<JSON::Number>("gc/path_removals_succeeded"));
  EXPECT_SOME_EQ(
      0u,
      metrics.at<JSON::Number>("gc/path_removals_failed"));

  Clock::resume();
}


TEST_F(GarbageCollectorTest, Unschedule)
{
  GarbageCollector gc("work_dir");

  // Attempt to unschedule a file that is not scheduled.
  AWAIT_ASSERT_FALSE(gc.unschedule("bogus"));

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
  AWAIT_ASSERT_TRUE(gc.unschedule(file2));
  AWAIT_ASSERT_TRUE(gc.unschedule(file3));
  AWAIT_ASSERT_TRUE(gc.unschedule(file1));

  // Advance the clock to ensure nothing was GCed.
  Clock::advance(Seconds(10));
  Clock::settle();

  // The unscheduling will have discarded the GC futures.
  AWAIT_DISCARDED(schedule1);
  AWAIT_DISCARDED(schedule2);
  AWAIT_DISCARDED(schedule3);

  EXPECT_TRUE(os::exists(file1));
  EXPECT_TRUE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));

  Clock::resume();
}


TEST_F(GarbageCollectorTest, Prune)
{
  GarbageCollector gc("work_dir");

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

  AWAIT_ASSERT_TRUE(gc.unschedule(file3));
  AWAIT_DISCARDED(schedule3);

  // Prune file1 and file2.
  gc.prune(Seconds(10));

  AWAIT_READY(schedule1);
  AWAIT_READY(schedule2);
  ASSERT_TRUE(schedule4.isPending());

  // Both file1 and file2 will have been removed.
  EXPECT_FALSE(os::exists(file1));
  EXPECT_FALSE(os::exists(file2));
  EXPECT_TRUE(os::exists(file3));
  EXPECT_TRUE(os::exists(file4));

  // Prune file4.
  gc.prune(Seconds(15));

  AWAIT_READY(schedule4);

  EXPECT_FALSE(os::exists(file4));

  Clock::resume();
}


class GarbageCollectorIntegrationTest : public MesosTest {};


// This test ensures that garbage collection does not remove
// the slave working directory after a slave restart.
TEST_F(GarbageCollectorIntegrationTest, Restart)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Need to create our own flags because we want to reuse them when
  // we (re)start the slave below.
  slave::Flags flags = CreateSlaveFlags();

  // Set the `executor_shutdown_grace_period` to a small value so that
  // the agent does not wait for executors to clean up for too long.
  flags.executor_shutdown_grace_period = Milliseconds(50);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Resources resources = Resources::parse(flags.resources.get()).get();
  double cpus = resources.get<Value::Scalar>("cpus")->value();
  double mem = resources.get<Value::Scalar>("mem")->value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, cpus, mem, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Ignore offerRescinded calls. The scheduler might receive it
  // because the slave might reregister due to ping timeout.
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Make sure directory exists. Need to do this AFTER getting a
  // status update for a task because the directory won't get created
  // until the task is launched. We get the slave ID from the
  // SlaveRegisteredMessage.
  const string& slaveDir = slave::paths::getSlavePath(
      flags.work_dir,
      slaveRegisteredMessage->slave_id());

  ASSERT_TRUE(os::exists(slaveDir));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(_, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Stop the slave with explicit shutdown as otherwise with
  // checkpointing the master will wait for the slave to reconnect.
  slave.get()->shutdown();

  AWAIT_READY(slaveLost);

  Clock::pause();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Wait for the agent to finish recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  Clock::advance(flags.gc_delay);

  Clock::settle();

  // By this time the old slave directory should not be cleaned up.
  ASSERT_TRUE(os::exists(slaveDir));

  Clock::resume();

  driver.stop();
  driver.join();
}


TEST_F(GarbageCollectorIntegrationTest, ExitedFramework)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  // Scheduler expectations.
  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Resources resources = Resources::parse(flags.resources.get()).get();
  double cpus = resources.get<Value::Scalar>("cpus")->value();
  double mem = resources.get<Value::Scalar>("mem")->value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, cpus, mem, "*"))
    .WillRepeatedly(Return());

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());      // Ignore subsequent updates.

  Future<Nothing> executorLaunched =
    FUTURE_DISPATCH(_, &Slave::executorLaunched);

  driver.start();

  // Wait until the slave has been notified about the start of the
  // executor. There is race where in a slave might get status updates
  // before it it notified about the start of the executor. This is
  // important in this test because if we don't wait and shutdown the
  // framework, it might so happen that 'executorLaunched' event is
  // received after the slave gets a 'shutdownFramework' leading to
  // shutdown and eventually gc of the executor and framework
  // directories. We want the gc to happen after we setup the
  // expectation on 'gc.schedule'.
  AWAIT_READY(executorLaunched);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  // Shutdown the framework.
  driver.stop();
  driver.join();

  Clock::pause();

  AWAIT_READY(shutdown);

  Clock::settle(); // Wait for Slave::shutdownExecutor to complete.

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Advance clock to kill executor via isolator.
  Clock::advance(flags.executor_shutdown_grace_period);

  Clock::settle();

  AWAIT_READY(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Clock::advance(flags.gc_delay);

  Clock::settle();

  // Framework's directory should be gc'ed by now.
  const string& frameworkDir = slave::paths::getFrameworkPath(
      flags.work_dir, slaveId, frameworkId);

  ASSERT_FALSE(os::exists(frameworkDir));

  process::UPID filesUpid("files", process::address());
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::NotFound().status,
      process::http::get(
          filesUpid,
          "browse",
          "path=" + frameworkDir,
          createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

  Clock::resume();
}


TEST_F(GarbageCollectorIntegrationTest, ExitedExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Resources resources = Resources::parse(flags.resources.get()).get();
  double cpus = resources.get<Value::Scalar>("cpus")->value();
  double mem = resources.get<Value::Scalar>("mem")->value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, cpus, mem, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Ignore offerRescinded calls. The scheduler might receive it
  // because the slave might reregister due to ping timeout.
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  const string& executorDir = slave::paths::getExecutorPath(
      flags.work_dir, slaveId, frameworkId.get(), DEFAULT_EXECUTOR_ID);

  ASSERT_TRUE(os::exists(executorDir));

  const string& latestDir = slave::paths::getExecutorLatestRunPath(
      flags.work_dir, slaveId, frameworkId.get(), DEFAULT_EXECUTOR_ID);

  process::UPID latest("files", process::address());
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::OK().status,
      process::http::get(
          latest,
          "browse",
          "path=" + latestDir,
          createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

  Clock::pause();

  // Killing the executor will cause the slave to schedule its
  // directory to get garbage collected.
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));

  // Kill the executor and inform the slave.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  AWAIT_READY(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  Clock::advance(flags.gc_delay);

  Clock::settle();

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));

  process::UPID files("files", process::address());
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::NotFound().status,
      process::http::get(
          files,
          "browse",
          "path=" + executorDir,
          createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::NotFound().status,
      process::http::get(
          latest,
          "browse",
          "path=" + latestDir,
          createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that task metadata and sandboxes are scheduled for GC
// when a task finishes, but the executor is still running.
TEST_F(GarbageCollectorIntegrationTest, LongLivedDefaultExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We need this for the agent's work directory and GC policy.
  slave::Flags flags = CreateSlaveFlags();

  // Turn on GC of nested container sandboxes by default.
  flags.gc_non_executor_container_sandboxes = true;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // Enable checkpointing, otherwise there will be no metadata to GC.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  // We launch two tasks for this test:
  //   * One will be a long-lived task to keep the executor alive.
  //   * One will be a short-lived task to exercise task metadata/sandbox GC.
  v1::TaskInfo longLivedTaskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo shortLivedTaskInfo =
    v1::createTask(agentId, resources, "exit 0");

  // There should be a total of 5 updates:
  //   * TASK_STARTING/RUNNING from the long-lived task,
  //   * TASK_STARTING/RUNNING/FINISHED from the short-lived task.
  testing::Sequence longTask;
  Future<v1::scheduler::Event::Update> longStartingUpdate;
  Future<v1::scheduler::Event::Update> longRunningUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(longLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(longTask)
    .WillOnce(DoAll(
        FutureArg<1>(&longStartingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(longLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(longTask)
    .WillOnce(DoAll(
        FutureArg<1>(&longRunningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  testing::Sequence shortTask;
  Future<v1::scheduler::Event::Update> shortStartingUpdate;
  Future<v1::scheduler::Event::Update> shortRunningUpdate;
  Future<v1::scheduler::Event::Update> shortFinishedUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortStartingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortRunningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortFinishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // There should be two directories scheduled for GC:
  // the short-lived task's metadata and sandbox.
  vector<Future<Nothing>> schedules = {
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule)
  };

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {
            v1::LAUNCH_GROUP(
                executorInfo, v1::createTaskGroupInfo({longLivedTaskInfo})),
            v1::LAUNCH_GROUP(
                executorInfo, v1::createTaskGroupInfo({shortLivedTaskInfo}))
          }));

  AWAIT_READY(collect(schedules));
  AWAIT_READY(longStartingUpdate);
  AWAIT_READY(longRunningUpdate);
  AWAIT_READY(shortStartingUpdate);
  AWAIT_READY(shortRunningUpdate);
  AWAIT_READY(shortFinishedUpdate);

  // Check that the short-lived task's metadata and sandbox exist.
  string shortLivedTaskPath = slave::paths::getTaskPath(
      slave::paths::getMetaRootDir(flags.work_dir),
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()),
      devolve(
          shortStartingUpdate->status()
            .container_status().container_id().parent()),
      devolve(shortLivedTaskInfo.task_id()));

  ASSERT_TRUE(os::exists(shortLivedTaskPath));

  string shortLivedSandboxPath = path::join(
      slave::paths::getExecutorRunPath(
          flags.work_dir,
            devolve(agentId),
            devolve(frameworkId),
            devolve(executorInfo.executor_id()),
            devolve(
                shortStartingUpdate->status()
                  .container_status().container_id().parent())),
      "containers",
      shortStartingUpdate->status().container_status().container_id().value());

  ASSERT_TRUE(os::exists(shortLivedSandboxPath));

  // Check another metadata directory that should only be GC'd after the
  // executor exits.
  string executorMetaPath = slave::paths::getExecutorPath(
      slave::paths::getMetaRootDir(flags.work_dir),
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  ASSERT_TRUE(os::exists(executorMetaPath));

  // Trigger garbage collection on the short-lived task's directories
  // and check that those are properly deleted.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::resume();

  ASSERT_FALSE(os::exists(shortLivedTaskPath));
  ASSERT_FALSE(os::exists(shortLivedSandboxPath));
  ASSERT_TRUE(os::exists(executorMetaPath));

  // Kill the remaining task and trigger garbage collection again.
  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&killedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Since this is the last executor belonging to the framework, we expect
  // multiple directories to be scheduled for GC:
  //   * Task, Executor container, Executor, and Framework metadata directories.
  //   * Executor sandbox and run directories.
  //   * Framework work directory.
  schedules = {
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule)
  };

  mesos.send(v1::createCallKill(frameworkId, longLivedTaskInfo.task_id()));

  AWAIT_READY(killedUpdate);
  EXPECT_EQ(v1::TASK_KILLED, killedUpdate->status().state());
  EXPECT_EQ(longLivedTaskInfo.task_id(), killedUpdate->status().task_id());

  AWAIT_READY(collect(schedules));

  // Trigger GC and then check one of the directories above.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::resume();

  ASSERT_FALSE(os::exists(executorMetaPath));
}


// This test verifies that task metadata and sandboxes are scheduled for GC
// when a task finishes, but the executor is still running. This version of
// the test restarts the agent to ensure recovered tasks are also scheduled
// for GC.
TEST_F(GarbageCollectorIntegrationTest, LongLivedDefaultExecutorRestart)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We need this for the agent's work directory and GC policy.
  slave::Flags flags = CreateSlaveFlags();

  // Turn on GC of nested container sandboxes by default.
  flags.gc_non_executor_container_sandboxes = true;

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id(process::ID::generate("agent"));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), id, flags, false);

  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // Enable checkpointing, otherwise there will be no metadata to GC.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  // We launch two tasks for this test:
  //   * One will be a long-lived task to keep the executor alive.
  //   * One will be a short-lived task to exercise task metadata/sandbox GC.
  v1::TaskInfo longLivedTaskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo shortLivedTaskInfo =
    v1::createTask(agentId, resources, "exit 0");

  // There should be a total of 5 updates:
  //   * TASK_STARTING/RUNNING from the long-lived task,
  //   * TASK_STARTING/RUNNING/FINISHED from the short-lived task.
  testing::Sequence longTask;
  Future<v1::scheduler::Event::Update> longStartingUpdate;
  Future<v1::scheduler::Event::Update> longRunningUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(longLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(longTask)
    .WillOnce(DoAll(
        FutureArg<1>(&longStartingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(longLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(longTask)
    .WillOnce(DoAll(
        FutureArg<1>(&longRunningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  testing::Sequence shortTask;
  Future<v1::scheduler::Event::Update> shortStartingUpdate;
  Future<v1::scheduler::Event::Update> shortRunningUpdate;
  Future<v1::scheduler::Event::Update> shortFinishedUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortStartingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortRunningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(shortLivedTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(shortTask)
    .WillOnce(DoAll(
        FutureArg<1>(&shortFinishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // There should be two directories scheduled for GC:
  // the short-lived task's metadata and sandbox.
  vector<Future<Nothing>> schedules = {
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule)
  };

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {
            v1::LAUNCH_GROUP(
                executorInfo, v1::createTaskGroupInfo({longLivedTaskInfo})),
            v1::LAUNCH_GROUP(
                executorInfo, v1::createTaskGroupInfo({shortLivedTaskInfo}))
          }));

  AWAIT_READY(collect(schedules));
  AWAIT_READY(longStartingUpdate);
  AWAIT_READY(longRunningUpdate);
  AWAIT_READY(shortStartingUpdate);
  AWAIT_READY(shortRunningUpdate);
  AWAIT_READY(shortFinishedUpdate);

  // Check that the short-lived task's metadata and sandbox exist.
  string shortLivedTaskPath = slave::paths::getTaskPath(
      slave::paths::getMetaRootDir(flags.work_dir),
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()),
      devolve(
          shortStartingUpdate->status()
            .container_status().container_id().parent()),
      devolve(shortLivedTaskInfo.task_id()));

  ASSERT_TRUE(os::exists(shortLivedTaskPath));

  string shortLivedSandboxPath = path::join(
      slave::paths::getExecutorRunPath(
          flags.work_dir,
            devolve(agentId),
            devolve(frameworkId),
            devolve(executorInfo.executor_id()),
            devolve(
                shortStartingUpdate->status()
                  .container_status().container_id().parent())),
      "containers",
      shortStartingUpdate->status().container_status().container_id().value());

  ASSERT_TRUE(os::exists(shortLivedSandboxPath));

  // Restart the agent to wipe out any scheduled GC.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);
  slave.get()->terminate();

  // The agent should reregister once recovery is complete, which also means
  // that any finished tasks metadata/sandboxes should be rescheduled for GC.
  slave = StartSlave(detector.get(), id, flags, false);
  ASSERT_SOME(slave);
  AWAIT_READY(slaveReregisteredMessage);

  // Trigger garbage collection on the short-lived task's directories
  // and check that those are properly deleted.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::resume();

  ASSERT_FALSE(os::exists(shortLivedTaskPath));
  ASSERT_FALSE(os::exists(shortLivedSandboxPath));
}


TEST_F(GarbageCollectorIntegrationTest, DiskUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Resources resources = Resources::parse(flags.resources.get()).get();
  double cpus = resources.get<Value::Scalar>("cpus")->value();
  double mem = resources.get<Value::Scalar>("mem")->value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, cpus, mem, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  const string& executorDir = slave::paths::getExecutorPath(
      flags.work_dir, slaveId, frameworkId.get(), DEFAULT_EXECUTOR_ID);

  ASSERT_TRUE(os::exists(executorDir));

  Clock::pause();

  // Killing the executor will cause the slave to schedule its
  // directory to get garbage collected.
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(1)); // Ignore TASK_LOST from killed executor.

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, slaveId, _));

  // Kill the executor and inform the slave.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  AWAIT_READY(schedule);

  Clock::settle(); // Wait for GarbageCollectorProcess::schedule to complete.

  // We advance the clock here so that the 'removalTime' of the
  // executor directory is definitely less than 'flags.gc_delay' in
  // the GarbageCollectorProcess 'GarbageCollector::prune()' gets
  // called (below). Otherwise, due to double comparison precision
  // in 'prune()' the directory might not be deleted.
  Clock::advance(Seconds(1));

  Future<Nothing> _checkDiskUsage =
    FUTURE_DISPATCH(_, &Slave::_checkDiskUsage);

  // Simulate a disk full message to the slave.
  process::dispatch(
      slave.get()->pid,
      &Slave::_checkDiskUsage,
      Try<double>(1.0 - slave::GC_DISK_HEADROOM));

  AWAIT_READY(_checkDiskUsage);

  Clock::settle(); // Wait for Slave::_checkDiskUsage to complete.

  // Executor's directory should be gc'ed by now.
  ASSERT_FALSE(os::exists(executorDir));

  process::UPID files("files", process::address());
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::NotFound().status,
      process::http::get(
          files,
          "browse",
          "path=" + executorDir,
          createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the launch of new executor will result in
// an unschedule of the framework work directory created by an old
// executor.
TEST_F(GarbageCollectorIntegrationTest, Unschedule)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  ExecutorInfo executor1 = createExecutorInfo("executor-1", "exit 1");
  ExecutorInfo executor2 = createExecutorInfo("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegistered);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Resources resources = Resources::parse(flags.resources.get()).get();
  double cpus = resources.get<Value::Scalar>("cpus")->value();
  double mem = resources.get<Value::Scalar>("mem")->value();

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(executor1, 1, cpus, mem, "*"));

  EXPECT_CALL(exec1, registered(_, _, _, _));

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  // TODO(benh/vinod): Would've been great to match the dispatch
  // against arguments here.
  // NOTE: Since Google Mock selects the last matching expectation
  // that is still active, the order of (un)schedule expectations
  // below are the reverse of the actual (un)schedule call order.

  // Schedule framework work directory.
  Future<Nothing> scheduleFrameworkWork =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Schedule top level executor work directory.
  Future<Nothing> scheduleExecutorWork =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Schedule executor run work directory.
  Future<Nothing> scheduleExecutorRunWork =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Unschedule framework work directory.
  Future<Nothing> unscheduleFrameworkWork =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::unschedule);

  // We ask the isolator to kill the first executor below.
  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(AtMost(2)); // Once for a TASK_LOST then once for TASK_RUNNING.

  // We use the killed executor/tasks resources to run another task.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(executor2, 1, cpus, mem, "*"));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(sched, executorLost(&driver, exec1.id, _, _));

  Clock::pause();

  // Kill the first executor.
  containerizer.destroy(frameworkId.get(), exec1.id);

  AWAIT_READY(scheduleExecutorRunWork);
  AWAIT_READY(scheduleExecutorWork);
  AWAIT_READY(scheduleFrameworkWork);

  // Speedup the allocator.
  while (unscheduleFrameworkWork.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(unscheduleFrameworkWork);

  Clock::resume();

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


#ifdef __linux__
// This test creates a persistent volume and runs a task which mounts the volume
// inside the sandbox, to simulate a dangling mount which agent failed to
// clean up (see MESOS-8830). We verify that GC process will unmount the
// dangling mount point successfully and report success in metrics.
TEST_F(GarbageCollectorIntegrationTest, ROOT_DanglingMount)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = strings::format("disk(%s):1024", DEFAULT_TEST_ROLE).get();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);

  ASSERT_SOME(slave);
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  string persistenceId = "persistence-id";
  string containerPath = "path";

  Resource volume = createPersistentVolume(
      Megabytes(1024),
      DEFAULT_TEST_ROLE,
      persistenceId,
      containerPath,
      None(),
      None(),
      frameworkInfo.principal());

  string mountPoint = "dangling";

  string hostPath = slave::paths::getPersistentVolumePath(
      flags.work_dir, DEFAULT_TEST_ROLE, persistenceId);

  string fileInVolume = "foo.txt";

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources(volume),
      "touch "+ path::join(containerPath, fileInVolume) + "; "
      "mkdir " + mountPoint + "; "
      "mount --bind " + hostPath + " " + mountPoint,
      None(),
      "test-task123",
      "test-task123");

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Future<Nothing> schedule = FUTURE_DISPATCH(
      _, &GarbageCollectorProcess::schedule);

  Future<Nothing> ack1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ack2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  ExecutorID executorId;
  executorId.set_value("test-task123");
  Result<string> _sandbox = os::realpath(slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      offer.slave_id(),
      frameworkId.get(),
      executorId));
  ASSERT_SOME(_sandbox);
  string sandbox = _sandbox.get();
  EXPECT_TRUE(os::exists(sandbox));

  // Wait for the task to create the dangling mount point.
  Timeout timeout = Timeout::in(process::TEST_AWAIT_TIMEOUT);
  while (!os::exists(path::join(sandbox, mountPoint)) ||
         !timeout.expired()) {
    os::sleep(Milliseconds(10));
  }

  ASSERT_TRUE(os::exists(path::join(sandbox, mountPoint)));

  AWAIT_READY(status2);
  ASSERT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_FINISHED, status2->state());

  AWAIT_READY(schedule);

  ASSERT_TRUE(os::exists(path::join(sandbox, mountPoint)));
  ASSERT_TRUE(os::exists(path::join(hostPath, fileInVolume)));

  AWAIT_READY(schedule);

  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();

  // Verify that GC metrics showes no failure.
  JSON::Object metrics = Metrics();

  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_pending"));
  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_succeeded"));
  ASSERT_EQ(1u, metrics.values.count("gc/path_removals_failed"));

  EXPECT_SOME_EQ(
      0u,
      metrics.at<JSON::Number>("gc/path_removals_pending"));

  EXPECT_SOME_EQ(
      0u,
      metrics.at<JSON::Number>("gc/path_removals_failed"));

  ASSERT_SOME(metrics.at<JSON::Number>("gc/path_removals_succeeded"));
  EXPECT_GT(
      metrics.at<JSON::Number>("gc/path_removals_succeeded")->as<unsigned>(),
      0u);

  ASSERT_FALSE(os::exists(path::join(sandbox, mountPoint)));
  ASSERT_TRUE(os::exists(path::join(hostPath, fileInVolume)));

  Clock::resume();
  driver.stop();
  driver.join();
}


// This is a regression test for MESOS-9966. It enables the agent
// flag `--gc_non_executor_container_sandboxes` and launches a
// nested container with checkpointing disabled, and then verifies
// that agent can recover successfully.
//
// TODO(qianzhang): For now, this test is Linux specific because
// the POSIX launcher is not able to destroy orphan containers
// after recovery, see MESOS-8771 for details.
TEST_F(GarbageCollectorIntegrationTest, ROOT_OrphanContainer)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Turn on GC of nested container sandboxes by default.
  slave::Flags flags = CreateSlaveFlags();
  flags.gc_non_executor_container_sandboxes = true;
  flags.launcher = "linux";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags, false);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // Disable checkpointing so the container launched by the framework will be
  // orphan after agent restarts.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(false);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(FutureArg<1>(&runningUpdate))
    .WillRepeatedly(Return());

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);

  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());
  EXPECT_TRUE(startingUpdate->status().has_timestamp());

  AWAIT_READY(runningUpdate);

  // Restart the agent.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slave.get()->terminate();

  slave = StartSlave(detector.get(), flags, false);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
