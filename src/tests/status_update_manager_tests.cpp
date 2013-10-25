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

#include <list>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "messages/messages.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;
using namespace mesos::internal::slave::paths;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;
using testing::SaveArg;

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


class StatusUpdateManagerTest: public MesosTest {};


TEST_F(StatusUpdateManagerTest, CheckpointStatusUpdate)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(_statusUpdateAcknowledgement);

  // Ensure that both the status update and its acknowledgement are
  // correctly checkpointed.
  Try<list<string> > found = os::find(flags.work_dir, TASK_UPDATES_FILE);
  ASSERT_SOME(found);
  ASSERT_EQ(1u, found.get().size());

  Try<int> fd = os::open(found.get().front(), O_RDONLY);
  ASSERT_SOME(fd);

  int updates = 0;
  int acks = 0;
  string uuid;
  Result<StatusUpdateRecord> record = None();
  while (true) {
    record = ::protobuf::read<StatusUpdateRecord>(fd.get());
    ASSERT_FALSE(record.isError());
    if (record.isNone()) { // Reached EOF.
      break;
    }

    if (record.get().type() == StatusUpdateRecord::UPDATE) {
      EXPECT_EQ(TASK_RUNNING, record.get().update().status().state());
      uuid = record.get().update().uuid();
      updates++;
    } else {
      EXPECT_EQ(uuid, record.get().uuid());
      acks++;
    }
  }

  ASSERT_EQ(1, updates);
  ASSERT_EQ(1, acks);

  close(fd.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(StatusUpdateManagerTest, RetryStatusUpdate)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get(), _);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that status update manager ignores
// duplicate ACK for an earlier update when it is waiting
// for an ACK for a later update. This could happen when the
// duplicate ACK is for a retried update.
TEST_F(StatusUpdateManagerTest, IgnoreDuplicateStatusUpdateAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
      .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop the first update, so that status update manager
  // resends the update.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get(), _);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);
  StatusUpdate update = statusUpdateMessage.get().update();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // This is the ACK for the retried update.
  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(ack);

  // Now send TASK_FINISHED update so that the status update manager
  // is waiting for its ACK, which it never gets because we drop the
  // update.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get(), _);

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
      slave.get(),
      &Slave::statusUpdateAcknowledgement,
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

  Shutdown();
}


// This test verifies that status update manager ignores
// unexpected ACK for an earlier update when it is waiting
// for an ACK for another update. We do this by dropping ACKs
// for the original update and sending a random ACK to the slave.
TEST_F(StatusUpdateManagerTest, IgnoreUnexpectedStatusUpdateAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
      .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), master.get(), _);

  // Drop the ACKs, so that status update manager
  // retries the update.
  DROP_PROTOBUFS(StatusUpdateAcknowledgementMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);
  StatusUpdate update = statusUpdateMessage.get().update();

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<Nothing> unexpectedAck =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  // Now send an ACK with a random UUID.
  process::dispatch(
      slave.get(),
      &Slave::statusUpdateAcknowledgement,
      update.slave_id(),
      frameworkId,
      update.status().task_id(),
      UUID::random().toBytes());

  AWAIT_READY(unexpectedAck);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the slave and status update manager
// properly handle duplicate terminal status updates, when the
// second update is received before the ACK for the first update.
// The proper behavior here is for the status update manager to
// drop the duplicate update.
TEST_F(StatusUpdateManagerTest, DuplicateTerminalUpdateBeforeAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Drop the first ACK from the scheduler to the slave.
  Future<StatusUpdateAcknowledgementMessage> statusUpdateAcknowledgementMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, slave.get());

  Future<Nothing> _statusUpdate =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdate);

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);

  EXPECT_EQ(TASK_FINISHED, status.get().state());

  AWAIT_READY(statusUpdateAcknowledgementMessage);

  // At this point the status update manager has enqueued
  // TASK_FINISHED update.
  AWAIT_READY(_statusUpdate);

  Future<Nothing> _statusUpdate2 =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdate);

  // Now send a TASK_KILLED update for the same task.
  TaskStatus status2 = status.get();
  status2.set_state(TASK_KILLED);
  execDriver->sendStatusUpdate(status2);

  // At this point the status update manager has enqueued
  // TASK_FINISHED and TASK_KILLED updates.
  AWAIT_READY(_statusUpdate2);

  // After we advance the clock, the scheduler should receive
  // the retried TASK_FINISHED update and acknowledge it. The
  // TASK_KILLED update should be dropped by the status update
  // manager, as the stream is already terminated.
  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL);
  Clock::settle();

  // Ensure the scheduler receives TASK_FINISHED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_FINISHED, update.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the slave and status update manager
// properly handle duplicate terminal status updates, when the
// second update is received after the ACK for the first update.
// The proper behavior here is for the status update manager to
// forward the duplicate update to the scheduler.
TEST_F(StatusUpdateManagerTest, DuplicateTerminalUpdateAfterAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

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
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(status);

  EXPECT_EQ(TASK_FINISHED, status.get().state());

  AWAIT_READY(_statusUpdateAcknowledgement);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdateAcknowledgement);

  Clock::pause();

  // Now send a TASK_KILLED update for the same task.
  TaskStatus status2 = status.get();
  status2.set_state(TASK_KILLED);
  execDriver->sendStatusUpdate(status2);

  // Ensure the scheduler receives TASK_KILLED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_KILLED, update.get().state());

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

  Shutdown();
}


// This test verifies that the slave and status update manager
// properly handle duplicate status updates, when the second
// update with the same UUID is received before the ACK for the
// first update. The proper behavior here is for the status update
// manager to drop the duplicate update.
TEST_F(StatusUpdateManagerTest, DuplicateUpdateBeforeAck)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;

  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

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
  Future<StatusUpdateAcknowledgementMessage> statusUpdateAcknowledgementMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, slave.get());

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), createTasks(offers.get()[0]));

  AWAIT_READY(statusUpdateMessage);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(statusUpdateAcknowledgementMessage);

  Future<Nothing> _statusUpdate =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdate);

  // Now resend the TASK_RUNNING update.
  process::post(slave.get(), statusUpdateMessage.get());

  // At this point the status update manager has handled
  // the duplicate status update.
  AWAIT_READY(_statusUpdate);

  // After we advance the clock, the status update manager should
  // retry the TASK_RUNING update and the scheduler should receive
  // and acknowledge it.
  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL);
  Clock::settle();

  // Ensure the scheduler receives TASK_FINISHED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}
