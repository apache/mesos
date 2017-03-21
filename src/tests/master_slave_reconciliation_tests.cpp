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

#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/allocator/allocator.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::protobuf;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;

using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class MasterSlaveReconciliationTest : public MesosTest {};


// This test verifies that a re-registering slave sends the terminal
// unacknowledged tasks for a terminal executor. This is required
// for the master to correctly reconcile its view with the slave's
// view of tasks. This test drops a terminal update to the master
// and then forces the slave to re-register.
TEST_F(MasterSlaveReconciliationTest, SlaveReregisterTerminatedExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<StatusUpdateAcknowledgementMessage> statusUpdateAcknowledgementMessage
    = FUTURE_PROTOBUF(
        StatusUpdateAcknowledgementMessage(),
        master.get()->pid,
        slave.get()->pid);

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Make sure the acknowledgement reaches the slave.
  AWAIT_READY(statusUpdateAcknowledgementMessage);

  // Drop the TASK_FINISHED status update sent to the master.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<ExitedExecutorMessage> executorExitedMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);

  TaskStatus finishedStatus;
  finishedStatus = status.get();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure the update was sent.
  AWAIT_READY(statusUpdateMessage);

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));

  // Now kill the executor.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  // We drop the 'UpdateFrameworkMessage' from the master to slave to
  // stop the status update manager from retrying the update that was
  // already sent due to the new master detection.
  DROP_PROTOBUFS(UpdateFrameworkMessage(), _, _);

  detector.appoint(master.get()->pid);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  driver.stop();
  driver.join();
}


// This test verifies that the master reconciles non-partition-aware
// tasks that are missing from a re-registering slave. In this case,
// we drop the RunTaskMessage, so the slave should send TASK_LOST.
TEST_F(MasterSlaveReconciliationTest, ReconcileLostTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // We now launch a task and drop the corresponding RunTaskMessage on
  // the slave, to ensure that only the master knows about this task.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(runTaskMessage);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Make sure the slave generated the TASK_LOST.
  AWAIT_READY(statusUpdateMessage);

  AWAIT_READY(status);

  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, status->reason());

  // Before we obtain the metrics, ensure that the master has finished
  // processing the status update so metrics have been updated.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_slave/reason_reconciliation"]);

  driver.stop();
  driver.join();
}


// This test verifies that the master reconciles partition-aware tasks
// that are missing from a re-registering slave. In this case, we drop
// the RunTaskMessage, so the slave should send TASK_DROPPED.
TEST_F(MasterSlaveReconciliationTest, ReconcileDroppedTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // We now launch a task and drop the corresponding RunTaskMessage on
  // the slave, to ensure that only the master knows about this task.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(runTaskMessage);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Make sure the slave generated the TASK_DROPPED.
  AWAIT_READY(statusUpdateMessage);

  AWAIT_READY(status);

  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_DROPPED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, status->reason());

  // Before we obtain the metrics, ensure that the master has finished
  // processing the status update so metrics have been updated.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(
      1u,
      stats.values["master/task_dropped/source_slave/reason_reconciliation"]);

  driver.stop();
  driver.join();
}


// This test verifies that the master reconciles tasks that are
// missing from a re-registering slave. In this case, we trigger
// a race between the slave re-registration message and the launch
// message. There should be no TASK_LOST / TASK_DROPPED.
// This was motivated by MESOS-1696.
TEST_F(MasterSlaveReconciliationTest, ReconcileRace)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, &containerizer);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Since the agent may have retried registration, we want to
  // ensure that any duplicate registrations are flushed before
  // we appoint the master again. Otherwise, the agent may
  // receive a stale registration message.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Trigger a re-registration of the slave and capture the message
  // so that we can spoof a race with a launch task message.
  DROP_PROTOBUFS(ReregisterSlaveMessage(), slave.get()->pid, master.get()->pid);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    DROP_PROTOBUF(
        ReregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  detector.appoint(master.get()->pid);

  AWAIT_READY(reregisterSlaveMessage);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  ExecutorDriver* executorDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&executorDriver));

  // Leave the task in TASK_STAGING.
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(launchTask);

  // Send the stale re-registration message, which does not contain
  // the task we just launched. This will trigger a reconciliation
  // by the master.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Prevent this from being dropped per the DROP_PROTOBUFS above.
  FUTURE_PROTOBUF(
      ReregisterSlaveMessage(),
      slave.get()->pid,
      master.get()->pid);

  process::post(
      slave.get()->pid,
      master.get()->pid,
      reregisterSlaveMessage.get());

  AWAIT_READY(slaveReregisteredMessage);

  // Neither the master nor the slave should send a TASK_LOST
  // as part of the reconciliation. We check this by calling
  // Clock::settle() to flush all pending events.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Now send TASK_FINISHED and make sure it's the only message
  // received by the scheduler.
  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskStatus taskStatus;
  taskStatus.mutable_task_id()->CopyFrom(task.task_id());
  taskStatus.set_state(TASK_FINISHED);
  executorDriver->sendStatusUpdate(taskStatus);

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FINISHED, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the slave reports pending tasks when
// re-registering, otherwise the master will report them as being
// lost.
TEST_F(MasterSlaveReconciliationTest, SlaveReregisterPendingTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  // No TASK_LOST updates should occur!
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  // We drop the _run dispatch to ensure the task remains
  // pending in the slave.
  Future<Nothing> _run = DROP_DISPATCH(slave.get()->pid, &Slave::_run);

  TaskInfo task1;
  task1.set_name("test task");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offers.get()[0].id(), {task1});

  AWAIT_READY(_run);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  Clock::pause();
  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that when the slave re-registers, the master
// does not send TASK_LOST update for a task that has reached terminal
// state but is waiting for an acknowledgement.
TEST_F(MasterSlaveReconciliationTest, SlaveReregisterTerminalTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Drop the status update from slave to the master, so that
  // the slave has a pending terminal update when it re-registers.
  DROP_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<Nothing> _statusUpdate = FUTURE_DISPATCH(_, &Slave::_statusUpdate);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore retried update due to update framework.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(_statusUpdate);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // The master should not send a TASK_LOST after the slave
  // re-registers. We check this by calling Clock::settle() so that
  // the only update the scheduler receives is the retried
  // TASK_FINISHED update.
  // NOTE: The status update manager resends the status update when
  // it detects a new master.
  Clock::pause();
  Clock::settle();

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FINISHED, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that when the slave re-registers, we correctly
// send the information about actively running frameworks.
TEST_F(MasterSlaveReconciliationTest, SlaveReregisterFrameworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Send an update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore retried update due to update framework.

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait until TASK_RUNNING of the task is received.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  // Expect to receive the 'ReregisterSlaveMessage' containing the
  // active frameworks.
  AWAIT_READY(reregisterSlave);

  EXPECT_EQ(1u, reregisterSlave->frameworks().size());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
