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

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "messages/messages.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


// TODO(benh): Move this into utils, make more generic, and use in
// other tests.
vector<TaskInfo> createTasks(const Offer& offer)
{
  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  return tasks;
}


class TaskStatusUpdateManagerTest: public MesosTest {};


TEST_F_TEMP_DISABLED_ON_WINDOWS(
    TaskStatusUpdateManagerTest, CheckpointStatusUpdate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Require flags to retrieve work_dir when recovering
  // the checkpointed data.
  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(_statusUpdateAcknowledgement);

  // Ensure that both the status update and its acknowledgement are
  // correctly checkpointed.
  Result<slave::state::State> state =
    slave::state::recover(slave::paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(state);
  ASSERT_SOME(state->slave);
  ASSERT_TRUE(state->slave->frameworks.contains(frameworkId.get()));

  slave::state::FrameworkState frameworkState =
    state->slave->frameworks.at(frameworkId.get());

  ASSERT_EQ(1u, frameworkState.executors.size());

  slave::state::ExecutorState executorState =
    frameworkState.executors.begin()->second;

  ASSERT_EQ(1u, executorState.runs.size());

  slave::state::RunState runState = executorState.runs.begin()->second;

  ASSERT_EQ(1u, runState.tasks.size());

  slave::state::TaskState taskState = runState.tasks.begin()->second;

  EXPECT_EQ(1u, taskState.updates.size());
  EXPECT_EQ(1u, taskState.acks.size());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(TaskStatusUpdateManagerTest, RetryStatusUpdate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get()->pid, _);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that status update manager ignores
// duplicate ACK for an earlier update when it is waiting
// for an ACK for a later update. This could happen when the
// duplicate ACK is for a retried update.
TEST_F(TaskStatusUpdateManagerTest, IgnoreDuplicateStatusUpdateAck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
      .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop the first update, so that status update manager
  // resends the update.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get()->pid, _);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);
  StatusUpdate update = statusUpdateMessage->update();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // This is the ACK for the retried update.
  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(ack);

  // Now send TASK_FINISHED update so that the status update manager
  // is waiting for its ACK, which it never gets because we drop the
  // update.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get()->pid, _);

  Future<Nothing> update2 = FUTURE_DISPATCH(_, &Slave::_statusUpdate);

  TaskStatus status2 = status.get();
  status2.set_state(TASK_FINISHED);

  execDriver->sendStatusUpdate(status2);

  AWAIT_READY(update2);

  // This is to catch the duplicate ack for TASK_RUNNING.
  Future<Nothing> duplicateAck =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  // Now send a duplicate ACK for the TASK_RUNNING update.
  process::dispatch(
      slave.get()->pid,
      &Slave::statusUpdateAcknowledgement,
      master.get()->pid,
      update.slave_id(),
      frameworkId,
      update.status().task_id(),
      update.uuid());

  AWAIT_READY(duplicateAck);

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that status update manager ignores
// unexpected ACK for an earlier update when it is waiting
// for an ACK for another update. We do this by dropping ACKs
// for the original update and sending a random ACK to the slave.
TEST_F(TaskStatusUpdateManagerTest, IgnoreUnexpectedStatusUpdateAck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
      .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), master.get()->pid, _);

  // Drop the ACKs, so that status update manager
  // retries the update.
  DROP_CALLS(mesos::scheduler::Call(),
             mesos::scheduler::Call::ACKNOWLEDGE,
             _,
             master.get()->pid);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);
  StatusUpdate update = statusUpdateMessage->update();

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<Nothing> unexpectedAck =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  // Now send an ACK with a random UUID.
  process::dispatch(
      slave.get()->pid,
      &Slave::statusUpdateAcknowledgement,
      master.get()->pid,
      update.slave_id(),
      frameworkId,
      update.status().task_id(),
      id::UUID::random().toBytes());

  AWAIT_READY(unexpectedAck);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the slave and status update manager
// properly handle duplicate terminal status updates, when the
// second update is received after the ACK for the first update.
// The proper behavior here is for the status update manager to
// forward the duplicate update to the scheduler.
TEST_F(TaskStatusUpdateManagerTest, DuplicateTerminalUpdateAfterAck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);

  EXPECT_EQ(TASK_FINISHED, status->state());

  AWAIT_READY(_statusUpdateAcknowledgement);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Clock::pause();

  // Now send a TASK_KILLED update for the same task.
  TaskStatus status2 = status.get();
  status2.set_state(TASK_KILLED);
  execDriver->sendStatusUpdate(status2);

  // Ensure the scheduler receives TASK_KILLED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_KILLED, update->state());

  // Ensure the slave properly handles the ACK.
  // Clock::settle() ensures that the slave successfully
  // executes Slave::_statusUpdateAcknowledgement().
  AWAIT_READY(_statusUpdateAcknowledgement2);
  Clock::settle();

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the slave and status update manager
// properly handle duplicate status updates, when the second
// update with the same UUID is received before the ACK for the
// first update. The proper behavior here is for the status update
// manager to drop the duplicate update.
TEST_F(TaskStatusUpdateManagerTest, DuplicateUpdateBeforeAck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Capture the first status update message.
  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Drop the first ACK from the scheduler to the slave.
  Future<StatusUpdateAcknowledgementMessage> statusUpdateAckMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, slave.get()->pid);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(statusUpdateAckMessage);

  Future<Nothing> ___statusUpdate =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::___statusUpdate);

  // Now resend the TASK_RUNNING update.
  process::post(slave.get()->pid, statusUpdateMessage.get());

  // At this point the status update manager has handled
  // the duplicate status update.
  AWAIT_READY(___statusUpdate);

  // After we advance the clock, the status update manager should
  // retry the TASK_RUNNING update and the scheduler should receive
  // and acknowledge it.
  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Ensure the scheduler receives TASK_FINISHED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the status update manager correctly includes
// the latest state of the task in status update.
TEST_F(TaskStatusUpdateManagerTest, LatestTaskState)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Signal when the first update is dropped.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<Nothing> ___statusUpdate = FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  driver.start();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure the status update manager handles the TASK_RUNNING update.
  AWAIT_READY(___statusUpdate);

  // Pause the clock to avoid status update manager from retrying.
  Clock::pause();

  Future<Nothing> ___statusUpdate2 =
    FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage->update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure the status update manager handles the TASK_FINISHED update.
  AWAIT_READY(___statusUpdate2);

  // Signal when the second update is dropped.
  Future<StatusUpdateMessage> statusUpdateMessage2 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  // Advance the clock for the status update manager to send a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(statusUpdateMessage2);

  // The update should correspond to TASK_RUNNING.
  ASSERT_EQ(TASK_RUNNING, statusUpdateMessage2->update().status().state());

  // The update should include TASK_FINISHED as the latest state.
  ASSERT_EQ(TASK_FINISHED,
            statusUpdateMessage2->update().latest_state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that if master receives a status update
// for an already terminated task it forwards it without
// changing the state of the task.
TEST_F(TaskStatusUpdateManagerTest, DuplicatedTerminalStatusUpdate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);

  EXPECT_EQ(TASK_FINISHED, status->state());

  AWAIT_READY(_statusUpdateAcknowledgement);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Clock::pause();

  // Now send a TASK_KILLED update for the same task.
  TaskStatus status2 = status.get();
  status2.set_state(TASK_KILLED);
  execDriver->sendStatusUpdate(status2);

  // Ensure the scheduler receives TASK_KILLED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_KILLED, update->state());

  // Ensure the slave properly handles the ACK.
  // Clock::settle() ensures that the slave successfully
  // executes Slave::_statusUpdateAcknowledgement().
  AWAIT_READY(_statusUpdateAcknowledgement2);

  // Verify the latest task status.
  Future<process::http::Response> tasks = process::http::get(
      master.get()->pid,
      "tasks",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, tasks);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", tasks);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(tasks->body);
  ASSERT_SOME(parse);

  Result<JSON::String> state = parse->find<JSON::String>("tasks[0].state");

  ASSERT_SOME_EQ(JSON::String("TASK_FINISHED"), state);

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
