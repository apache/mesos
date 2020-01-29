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

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registry_operations.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;

using std::vector;

using testing::_;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

class ReconciliationTest : public MesosTest {};


// This test verifies that reconciliation sends the latest task
// status, when the task state does not match between the framework
// and the master.
TEST_F(ReconciliationTest, TaskStateMismatch)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());

  EXPECT_TRUE(update->has_slave_id());

  const TaskID taskId = update->task_id();
  const SlaveID slaveId = update->slave_id();

  // If framework has different state, current state should be reported.
  Future<TaskStatus> update2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update2));

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(taskId);
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  AWAIT_READY(update2);
  EXPECT_EQ(TASK_RUNNING, update2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update2->reason());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that task reconciliation results in a status
// update, when the task state matches between the framework and the
// master.
// TODO(bmahler): Now that the semantics have changed, consolidate
// these tests? There's no need to test anything related to the
// task state difference between the master and the framework.
TEST_F(ReconciliationTest, TaskStateMatch)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());
  EXPECT_TRUE(update->has_slave_id());

  const TaskID taskId = update->task_id();
  const SlaveID slaveId = update->slave_id();

  // Framework should not receive a status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(taskId);
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> update2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update2));

  driver.reconcileTasks({status});

  AWAIT_READY(update2);
  EXPECT_EQ(TASK_RUNNING, update2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update2->reason());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that reconciliation of a task that belongs to an
// unknown slave results in TASK_UNKNOWN if the framework has enabled
// the PARTITION_AWARE capability.
TEST_F(ReconciliationTest, UnknownSlave)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Create a task status with a random slave id and task id.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->set_value(id::UUID::random().toString());
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  // Framework should receive TASK_UNKNOWN because the slave is unknown.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_UNKNOWN, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  EXPECT_FALSE(update->has_unreachable_time());

  driver.stop();
  driver.join();
}


// This test verifies that reconciliation of an unknown task that
// belongs to a known slave results in TASK_LOST if the framework is
// not partition-aware.
TEST_F(ReconciliationTest, UnknownTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait for the slave to register and get the slave id.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Create a task status with a random task id.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  // Framework should receive TASK_LOST for an unknown task.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_LOST, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  EXPECT_FALSE(update->has_unreachable_time());

  driver.stop();
  driver.join();
}


// This test verifies that reconciliation of an unknown task that
// belongs to a known slave results in TASK_GONE if the framework is
// partition-aware.
TEST_F(ReconciliationTest, UnknownTaskPartitionAware)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait for the slave to register and get the slave id.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Create a task status with a random task id.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  // Framework should receive TASK_GONE for an unknown task.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_GONE, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  EXPECT_FALSE(update->has_unreachable_time());

  driver.stop();
  driver.join();
}


// This test verifies that the killTask request of an unknown task
// results in reconciliation. In this case, the task is unknown
// and there are no transitional slaves.
TEST_F(ReconciliationTest, UnknownKillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Create a task status with a random task id.
  TaskID taskId;
  taskId.set_value(id::UUID::random().toString());

  driver.killTask(taskId);

  // Framework should receive TASK_LOST for unknown task.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_LOST, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  EXPECT_FALSE(update->has_unreachable_time());

  driver.stop();
  driver.join();
}


// This test verifies that explicit reconciliation does not return any
// results for tasks running on an agent that has been recovered from
// the registry after master failover but has not yet reregistered.
TEST_F(ReconciliationTest, RecoveredAgent)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait for the slave to register and get the slave id.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Stop the master.
  master->reset();

  // Stop the slave.
  slave.get()->terminate();
  slave->reset();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  // Framework should not receive any task status updates.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.start();

  // Wait for the framework to register.
  AWAIT_READY(frameworkId);

  // Do reconciliation before the agent has attempted to reregister.
  // This should not yield any results.
  Future<mesos::scheduler::Call> reconcileCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::RECONCILE, _, _);

  // Reconcile for a random task ID on the slave.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  // Make sure the master received the reconcile call.
  AWAIT_READY(reconcileCall);

  // The Clock::settle() will ensure that framework would receive
  // a status update if it is sent by the master. In this test it
  // shouldn't receive any.
  Clock::pause();
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that explicit reconciliation does not return any
// results for tasks running on an agent that has been recovered from
// the registry after master failover, where the agent has started the
// reregistration process but has not completed it yet.
TEST_F(ReconciliationTest, RecoveredAgentReregistrationInProgress)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Wait for the slave to register and get the slave id.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Stop the master.
  master->reset();

  // Stop the slave.
  slave.get()->terminate();
  slave->reset();

  // Restart master with a mock authorizer to block agent state transitioning.
  MockAuthorizer authorizer;
  master = StartMaster(&authorizer, masterFlags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  // Framework should not receive any task status updates.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.start();

  // Wait for the framework to register.
  AWAIT_READY(frameworkId);

  // Intercept agent authorization.
  Future<Nothing> authorize;
  Promise<bool> promise; // Never satisfied.
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  // Restart the slave.
  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Wait for the slave to start reregistration.
  AWAIT_READY(authorize);

  Future<mesos::scheduler::Call> reconcileCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::RECONCILE, _, _);

  // Reconcile for a random task ID on the slave.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  // Make sure the master received the reconcile call.
  AWAIT_READY(reconcileCall);

  // The Clock::settle() will ensure that the framework receives a
  // status update if it is sent by the master. In this test it
  // shouldn't receive any.
  Clock::pause();
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test ensures that when an agent has started but not finished
// the unregistration process, explicit reconciliation indicates that
// the agent is still registered.
//
// TODO(alexr): Enable after MESOS-8210 is resolved.
TEST_F(ReconciliationTest, DISABLED_RemovalInProgress)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait for the slave to register and get the slave id.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  Future<UnregisterSlaveMessage> unregisterSlaveMessage =
    FUTURE_PROTOBUF(
        UnregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  // Intercept the next registrar operation; this should be the
  // registry operation that unregisters the slave.
  Future<Owned<master::RegistryOperation>> unregister;
  Future<Nothing> unregisterStarted;
  Promise<bool> promise; // Never satisfied.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&unregister),
                    Return(promise.future())));

  // Cause the slave to shutdown gracefully.
  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(unregisterSlaveMessage);

  AWAIT_READY(unregister);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::RemoveSlave*>(unregister->get()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  // Wait for the framework to register.
  AWAIT_READY(frameworkId);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Reconcile for a random task ID on the slave.
  TaskStatus status;
  status.mutable_task_id()->set_value(id::UUID::random().toString());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  driver.reconcileTasks({status});

  AWAIT_READY(update);
  EXPECT_EQ(TASK_LOST, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  EXPECT_FALSE(update->has_unreachable_time());

  driver.stop();
  driver.join();
}


// This test ensures that an implicit reconciliation request results
// in updates for all non-terminal tasks known to the master.
TEST_F(ReconciliationTest, ImplicitNonTerminalTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Launch a framework and get a task running.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());
  EXPECT_TRUE(update->has_slave_id());

  // When making an implicit reconciliation request, the non-terminal
  // task should be sent back.
  Future<TaskStatus> update2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update2));

  driver.reconcileTasks({});

  AWAIT_READY(update2);
  EXPECT_EQ(TASK_RUNNING, update2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update2->reason());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that the master does not send updates for
// terminal tasks during an implicit reconciliation request.
// TODO(bmahler): Soon the master will keep non-acknowledged
// tasks, and this test may break.
TEST_F(ReconciliationTest, ImplicitTerminalTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Launch a framework and get a task terminal.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_FINISHED, update->state());
  EXPECT_TRUE(update->has_slave_id());

  // Framework should not receive any further updates.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<mesos::scheduler::Call> reconcileCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::RECONCILE, _, _);

  Clock::pause();

  // When making an implicit reconciliation request, the master
  // should not send back terminal tasks.
  driver.reconcileTasks({});

  // Make sure the master received the reconcile call.
  AWAIT_READY(reconcileCall);

  // The Clock::settle() will ensure that framework would receive
  // a status update if it is sent by the master. In this test it
  // shouldn't receive any.
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test ensures that the master responds with the latest state
// for tasks that are terminal at the master, but have not been
// acknowledged by the framework. See MESOS-1389.
TEST_F(ReconciliationTest, UnacknowledgedTerminalTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Launch a framework and get a task into a terminal state.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> update1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update1));

  // Prevent the slave from retrying the status update by
  // only allowing a single update through to the master.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get()->pid);
  FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  // Drop the status update acknowledgements to ensure that the
  // task remains terminal and unacknowledged in the master.
  DROP_CALLS(mesos::scheduler::Call(),
             mesos::scheduler::Call::ACKNOWLEDGE,
             _,
             master.get()->pid);

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(update1);
  EXPECT_EQ(TASK_FINISHED, update1->state());
  EXPECT_TRUE(update1->has_slave_id());

  // Framework should receive a TASK_FINISHED update, since the
  // master did not receive the acknowledgement.
  Future<TaskStatus> update2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update2));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.reconcileTasks({});

  AWAIT_READY(update2);
  EXPECT_EQ(TASK_FINISHED, update2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update2->reason());
  EXPECT_TRUE(update2->has_slave_id());

  driver.stop();
  driver.join();
}


// This test verifies that when the task's latest and status update
// states differ, master responds to reconciliation request with the
// status update state.
TEST_F(ReconciliationTest, ReconcileStatusUpdateTaskState)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start a slave.
  slave::Flags agentFlags = CreateSlaveFlags();
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector slaveDetector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&slaveDetector, &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Start a scheduler.
  MockScheduler sched;
  StandaloneMasterDetector schedulerDetector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 1024, "*"))
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

  // Pause the clock to avoid status update retries.
  Clock::pause();

  // Advance clock to trigger agent registration and offers.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure task status update manager handles TASK_RUNNING update.
  AWAIT_READY(___statusUpdate);

  Future<Nothing> ___statusUpdate2 =
    FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage->update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure task status update manager handles TASK_FINISHED update.
  AWAIT_READY(___statusUpdate2);

  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(Return());

  // Simulate master failover by restarting the master.
  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Clock::resume();

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Re-register the framework.
  schedulerDetector.appoint(master.get()->pid);

  AWAIT_READY(registered);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(
        SlaveReregisteredMessage(),
        master.get()->pid,
        slave.get()->pid);

  // Drop all updates to the second master.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get()->pid);

  // Re-register the slave.
  slaveDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Framework should receive a TASK_RUNNING update, since that is the
  // latest status update state of the task.
  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Reconcile the state of the task.
  driver.reconcileTasks({});

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(ReconciliationTest, PartitionedAgentThenMasterFailover)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  StandaloneMasterDetector schedulerDetector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Launch `task` using `sched`.
  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck2);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unhealthy. We then advance the clock -- this shouldn't
  // do anything, but it ensures that the `unreachable_time` we check
  // below is computed at the right time.
  TimeInfo partitionTime = protobuf::getCurrentTime();

  Clock::advance(Milliseconds(100));

  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());
  EXPECT_EQ(task.task_id(), lostStatus->task_id());
  EXPECT_EQ(slaveId, lostStatus->slave_id());
  EXPECT_EQ(partitionTime, lostStatus->unreachable_time());

  AWAIT_READY(slaveLost);

  // Do the first explicit reconciliation before restarting the master.
  TaskStatus status1;
  status1.mutable_task_id()->CopyFrom(task.task_id());
  status1.mutable_slave_id()->CopyFrom(slaveId);
  status1.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status1});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_LOST, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_EQ(partitionTime, reconcileUpdate1->unreachable_time());

  // Drop all subsequent messages from the slave, since they aren't
  // useful anymore.
  DROP_MESSAGES(_, slave.get()->pid, _);

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate master failover by restarting the master.
  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Re-register the framework. Note that we don't update the slave's
  // master detector, so it does not try to reregister.
  schedulerDetector.appoint(master.get()->pid);

  AWAIT_READY(registered);

  // Do a second explicit reconciliation; we expect to observe the same data.
  TaskStatus status2;
  status2.mutable_task_id()->CopyFrom(task.task_id());
  status2.mutable_slave_id()->CopyFrom(slaveId);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status2});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_LOST, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());
  EXPECT_EQ(partitionTime, reconcileUpdate2->unreachable_time());

  Clock::resume();

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
