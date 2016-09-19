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

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;

using process::http::OK;
using process::http::Response;

using std::vector;

using testing::_;
using testing::AtMost;
using testing::Eq;
using testing::Return;

using ::testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {


class PartitionTest : public MesosTest,
                      public WithParamInterface<bool> {};


// The Registrar tests are parameterized by "strictness".
INSTANTIATE_TEST_CASE_P(Strict, PartitionTest, ::testing::Bool());


// This test checks that a scheduler gets a slave lost
// message for a partitioned slave.
TEST_P(PartitionTest, PartitionedSlave)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  Clock::pause();

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now advance through the PINGs.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(slaveLost);

  slave.get()->terminate();
  slave->reset();

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks that a slave can reregister with the master after
// a partition, and that PARTITION_AWARE tasks running on the slave
// continue to run.
TEST_P(PartitionTest, ReregisterSlavePartitionAware)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  // Start a scheduler. The scheduler has the PARTITION_AWARE
  // capability, so we expect its tasks to continue running when the
  // partitioned agent reregisters.
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
  ASSERT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus.get().state());
  EXPECT_EQ(task.task_id(), runningStatus.get().task_id());

  const SlaveID slaveId = runningStatus.get().slave_id();

  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  // TODO(neilc): Update this when TASK_UNREACHABLE is introduced.
  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_LOST, unreachableStatus.get().state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus.get().reason());
  EXPECT_EQ(task.task_id(), unreachableStatus.get().task_id());
  EXPECT_EQ(slaveId, unreachableStatus.get().slave_id());

  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  // We now complete the partition on the slave side as well. We
  // simulate a master loss event, which would normally happen during
  // a network partition. The slave should then reregister with the
  // master.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregistered);

  // Perform explicit reconciliation; the task should still be running.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_RUNNING, reconcileUpdate.get().state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate.get().reason());

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test checks that a slave can reregister with the master after
// a partition, and that non-PARTITION_AWARE tasks running on the
// slave are shutdown.
TEST_P(PartitionTest, ReregisterSlaveNotPartitionAware)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  // Start a scheduler. The scheduler is not PARTITION_AWARE, so we
  // expect its tasks to be shutdown when the partitioned agent
  // reregisters.
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
  ASSERT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus.get().state());
  EXPECT_EQ(task.task_id(), runningStatus.get().task_id());

  const SlaveID slaveId = runningStatus.get().slave_id();

  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  // Note that we expect to get `slaveLost` callbacks in both
  // schedulers, regardless of PARTITION_AWARE.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  // The scheduler should see TASK_LOST because it is not
  // PARTITION_AWARE.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus.get().state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus.get().reason());
  EXPECT_EQ(task.task_id(), lostStatus.get().task_id());
  EXPECT_EQ(slaveId, lostStatus.get().slave_id());

  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  // We now complete the partition on the slave side as well. We
  // simulate a master loss event, which would normally happen during
  // a network partition. The slave should then reregister with the
  // master.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregistered);

  // Perform explicit reconciliation. The task should not be running
  // (TASK_LOST) because the framework is not PARTITION_AWARE.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_LOST, reconcileUpdate.get().state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate.get().reason());

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test checks how Mesos behaves when a slave is removed because
// it fails health checks, and then the slave sends a status update
// (because it does not realize that it is partitioned from the
// master's POV). In prior Mesos versions, the master would shutdown
// the slave in this situation. In Mesos >= 1.1, the master will drop
// the status update; the slave will eventually try to reregister.
TEST_P(PartitionTest, PartitionedSlaveStatusUpdates)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Drop both PINGs from master to slave and PONGs from slave to
  // master. Note that we don't match on the master / slave PIDs
  // because it's actually the SlaveObserver Process that sends pings
  // and receives pongs.
  DROP_PROTOBUFS(PingSlaveMessage(), _, _);
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now, induce a partition of the slave by having the master timeout
  // the slave. The master will remove the slave; the slave will also
  // realize that it hasn't seen any pings from the master and try to
  // reregister. We don't want to let the slave reregister yet, so we
  // drop the first message in the reregistration protocol, which is
  // AuthenticateMessage since agent auth is enabled.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  for (size_t i = 0; i < masterFlags.max_agent_ping_timeouts; i++) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  // Slave will try to authenticate for reregistration; message dropped.
  AWAIT_READY(authenticateMessage);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  // At this point, the slave still thinks it's registered, so we
  // simulate a status update coming from the slave.
  TaskID taskId1;
  taskId1.set_value("task_id1");

  const StatusUpdate& update1 = protobuf::createStatusUpdate(
      frameworkId.get(),
      slaveId,
      taskId1,
      TASK_RUNNING,
      TaskStatus::SOURCE_SLAVE,
      UUID::random());

  StatusUpdateMessage message1;
  message1.mutable_update()->CopyFrom(update1);
  message1.set_pid(stringify(slave.get()->pid));

  // The scheduler should not receive the status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  process::post(master.get()->pid, message1);
  Clock::settle();

  // Advance the clock so that the slaves notices that it still hasn't
  // seen any pings from the master, which will cause it to try to
  // reregister again. This time reregistration should succeed.
  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  for (size_t i = 0; i < masterFlags.max_agent_ping_timeouts; i++) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  AWAIT_READY(slaveReregistered);

  // Since the slave has reregistered, a status update from the slave
  // should now be forwarded to the scheduler.
  Future<StatusUpdateMessage> statusUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get()->pid, _);

  TaskID taskId2;
  taskId2.set_value("task_id2");

  const StatusUpdate& update2 = protobuf::createStatusUpdate(
      frameworkId.get(),
      slaveId,
      taskId2,
      TASK_RUNNING,
      TaskStatus::SOURCE_SLAVE,
      UUID::random());

  StatusUpdateMessage message2;
  message2.mutable_update()->CopyFrom(update2);
  message2.set_pid(stringify(slave.get()->pid));

  process::post(master.get()->pid, message2);

  AWAIT_READY(statusUpdate);
  EXPECT_EQ(taskId2, statusUpdate->update().status().task_id());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks how Mesos behaves when a slave is removed, and
// then the slave sends an ExitedExecutorMessage (because it does not
// realize it is partitioned from the master's POV). In prior Mesos
// versions, the master would shutdown the slave in this situation. In
// Mesos >= 1.1, the master will drop the message; the slave will
// eventually try to reregister.
TEST_P(PartitionTest, PartitionedSlaveExitedExecutor)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));\

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  // Launch a task. This allows us to have the slave send an
  // ExitedExecutorMessage.
  TaskInfo task = createTask(offers.get()[0], "sleep 60", DEFAULT_EXECUTOR_ID);

  // Set up the expectations for launching the task.
  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the status updates from the slave.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get()->pid);

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  // The master will notify the framework of the lost task.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus.get().state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus.get().reason());

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  EXPECT_CALL(sched, executorLost(&driver, _, _, _))
    .Times(0);

  // Induce an ExitedExecutorMessage from the slave.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  // The master will drop the ExitedExecutorMessage. We do not
  // currently support reliable delivery of ExitedExecutorMessages, so
  // the message will not be delivered if/when the slave reregisters.
  //
  // TODO(neilc): Update this test to check for reliable delivery once
  // MESOS-4308 is fixed.
  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}


class OneWayPartitionTest : public MesosTest {};


// This test verifies that if master --> slave socket closes and the
// slave is not aware of it (i.e., one way network partition), slave
// will re-register with the master.
TEST_F(OneWayPartitionTest, MasterToSlave)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  // Ensure a ping reaches the slave.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  AWAIT_READY(ping);

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::deactivateSlave);

  // Inject a slave exited event at the master causing the master
  // to mark the slave as disconnected. The slave should not notice
  // it until the next ping is received.
  process::inject::exited(slaveRegisteredMessage.get().to, master.get()->pid);

  // Wait until master deactivates the slave.
  AWAIT_READY(deactivateSlave);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Ensure the slave observer marked the slave as deactivated.
  Clock::pause();
  Clock::settle();

  // Let the slave observer send the next ping.
  Clock::advance(masterFlags.agent_ping_timeout);

  // Slave should re-register.
  AWAIT_READY(slaveReregisteredMessage);
}


// This test verifies that if master --> framework socket closes and the
// framework is not aware of it (i.e., one way network partition), all
// subsequent calls from the framework after the master has marked it as
// disconnected would result in an error message causing the framework to abort.
TEST_F(OneWayPartitionTest, MasterToScheduler)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  AWAIT_READY(registered);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  // Simulate framework disconnection. This should result in an error message.
  ASSERT_TRUE(process::inject::exited(
      frameworkRegisteredMessage.get().to, master.get()->pid));

  AWAIT_READY(error);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
