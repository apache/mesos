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

#include <utility>
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


// This test verifies that a reregistering slave sends the terminal
// unacknowledged tasks for a terminal executor. This is required
// for the master to correctly reconcile its view with the slave's
// view of tasks. This test drops a terminal update to the master
// and then forces the slave to reregister.
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
  // stop the task status update manager from retrying the update that
  // was already sent due to the new master detection.
  DROP_PROTOBUFS(UpdateFrameworkMessage(), _, _);

  detector.appoint(master.get()->pid);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  driver.stop();
  driver.join();
}


// This test verifies that the master reconciles non-partition-aware
// tasks that are missing from a reregistering slave. In this case,
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

  ASSERT_FALSE(offers->empty());

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
// that are missing from a reregistering slave. In this case, we drop
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

  ASSERT_FALSE(offers->empty());

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


// This test verifies that the master reconciles operations that are missing
// from a reregistering slave. In this case, we drop the ApplyOperationMessage
// and expect the master to send a ReconcileOperationsMessage after the slave
// reregisters.
TEST_F(MasterSlaveReconciliationTest, ReconcileDroppedOperation)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  // Since any out-of-sync operation state in `UpdateSlaveMessage` triggers a
  // reconciliation, await the message from the initial agent registration
  // sequence beforce continuing. Otherwise we risk the master reconciling with
  // the agent before we fail over the master.
  AWAIT_READY(updateSlaveMessage);

  // Register the framework in a non-`*` role so it can reserve resources.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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

  // We prevent the operation from reaching the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // Perform a reserve operation on the offered resources.
  // This will trigger an `ApplyOperationMessage`.
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers->at(0);

  Resources reservedResources = offer.resources();
  reservedResources =
    reservedResources.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  driver.acceptOffers({offer.id()}, {RESERVE(reservedResources)});

  AWAIT_READY(applyOperationMessage);

  // We expect the master to detect the missing operation when the
  // slave reregisters and to reconcile the operations on that slave.
  Future<ReconcileOperationsMessage> reconcileOperationsMessage =
    FUTURE_PROTOBUF(ReconcileOperationsMessage(), _, _);

  // Simulate a master failover to trigger slave reregistration.
  detector.appoint(master.get()->pid);

  AWAIT_READY(reconcileOperationsMessage);

  ASSERT_EQ(1, reconcileOperationsMessage->operations_size());
  EXPECT_EQ(
      applyOperationMessage->operation_uuid(),
      reconcileOperationsMessage->operations(0).operation_uuid());
}

// The master reconciles operations that are missing from a re-registering
// agent.
//
// In this case, the `ApplyOperationMessage` is dropped, so the agent should
// respond with a OPERATION_DROPPED operation status update.
//
// This test verifies that if an operation ID is set, the framework receives
// the OPERATION_DROPPED operation status update.
//
// This is a regression test for MESOS-8784.
TEST_F(
    MasterSlaveReconciliationTest,
    ForwardOperationDroppedAfterExplicitReconciliation)
{
  Clock::pause();

  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  mesos::internal::slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the agent to register.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.

  v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  Owned<v1::TestResourceProvider> resourceProvider(
      new v1::TestResourceProvider(resourceProviderInfo, v1::Resources(disk)));

  Future<v1::ResourceProviderID> resourceProviderId =
    resourceProvider->process->id();

  // Make the mock resource provider answer to reconciliation events with
  // OPERATION_DROPPED operation status updates.
  auto reconcileOperations =
    [&resourceProvider, &resourceProviderId](
        const v1::resource_provider::Event::ReconcileOperations& reconcile) {
      // NOTE: We do not use `AWAIT_READY` here since it
      // would deadlock with below `Invoke` invocation.
      ASSERT_TRUE(resourceProviderId.isReady());

      foreach (const v1::UUID& operationUuid, reconcile.operation_uuids()) {
        v1::resource_provider::Call call;

        call.set_type(v1::resource_provider::Call::UPDATE_OPERATION_STATUS);
        call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

        v1::resource_provider::Call::UpdateOperationStatus*
          updateOperationStatus = call.mutable_update_operation_status();

        updateOperationStatus->mutable_status()->set_state(
            v1::OPERATION_DROPPED);

        updateOperationStatus->mutable_operation_uuid()->CopyFrom(
            operationUuid);

        updateOperationStatus->mutable_status()
          ->mutable_resource_provider_id()
          ->CopyFrom(resourceProviderId.get());

        resourceProvider->send(call);
      }
    };

  EXPECT_CALL(*resourceProvider->process, reconcileOperations(_))
    .WillOnce(Invoke(reconcileOperations));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // NOTE: We need to resume the clock so that the resource provider can
  // fully register.
  Clock::resume();

  ContentType contentType = ContentType::PROTOBUF;

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  Clock::pause();

  // Start a v1 framework.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  // Ignore heartbeats.
  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return());

  Future<v1::scheduler::Event::Offers> offers;

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  v1::scheduler::TestMesos mesos(master.get()->pid, contentType, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // We'll drop the `ApplyOperationMessage` from the master to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), master.get()->pid, _);

  v1::Resources resources =
    v1::Resources(offer.resources()).filter([](const v1::Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  v1::Resource reserved = *(resources.begin());
  reserved.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId, offer, {v1::RESERVE(reserved, operationId)}));

  AWAIT_READY(applyOperationMessage);

  Future<v1::scheduler::Event::UpdateOperationStatus> operationDroppedUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&operationDroppedUpdate));

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector->appoint(master.get()->pid);

  // Advance the clock, so that the agent re-registers.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait for the framework to receive the OPERATION_DROPPED update.
  AWAIT_READY(operationDroppedUpdate);

  EXPECT_EQ(operationId, operationDroppedUpdate->status().operation_id());
  EXPECT_EQ(v1::OPERATION_DROPPED, operationDroppedUpdate->status().state());
  EXPECT_TRUE(metricEquals("master/operations/dropped", 1));
}

// This test verifies that the master reconciles tasks that are
// missing from a reregistering slave. In this case, we trigger
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
  ASSERT_FALSE(offers->empty());

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
// reregistering, otherwise the master will report them as being
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
  ASSERT_FALSE(offers->empty());

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


// This test verifies that when the slave reregisters, the master
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
  ASSERT_FALSE(offers->empty());

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
  // the slave has a pending terminal update when it reregisters.
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
  // reregisters. We check this by calling Clock::settle() so that
  // the only update the scheduler receives is the retried
  // TASK_FINISHED update.
  // NOTE: The task status update manager resends the status update
  // when it detects a new master.
  Clock::pause();
  Clock::settle();

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FINISHED, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that when the slave reregisters, we correctly
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
  ASSERT_FALSE(offers->empty());

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

  EXPECT_EQ(1, reregisterSlave->frameworks().size());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that when reregistering, the slave sends the
// executor ID of a non-command executor task, but not the one of a
// command executor task. We then check that the master's API has
// task IDs absent only for the command executor case.
//
// This was motivated by MESOS-8135.
TEST_F(MasterSlaveReconciliationTest, SlaveReregisterTaskExecutorIds)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  const Offer& offer = offers->front();
  const SlaveID& slaveId = offer.slave_id();

  Resources resources = Resources::parse(defaultTaskResourcesString).get();

  TaskInfo commandExecutorTask =
    createTask(slaveId, resources, SLEEP_COMMAND(1000));

  TaskInfo defaultExecutorTask =
    createTask(slaveId, resources, SLEEP_COMMAND(1000));

  ExecutorInfo defaultExecutorInfo;
  defaultExecutorInfo.set_type(ExecutorInfo::DEFAULT);
  defaultExecutorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  defaultExecutorInfo.mutable_framework_id()->CopyFrom(frameworkId.get());
  defaultExecutorInfo.mutable_resources()->CopyFrom(resources);

  // We expect two TASK_STARTING and two TASK_RUNNING updates.
  vector<Future<TaskStatus>> taskStatuses(4);

  {
    // This variable doesn't have to be used explicitly.
    testing::InSequence inSequence;

    foreach (Future<TaskStatus>& taskStatus, taskStatuses) {
      EXPECT_CALL(sched, statusUpdate(&driver, _))
        .WillOnce(FutureArg<1>(&taskStatus));
    }

    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillRepeatedly(Return()); // Ignore subsequent updates.
  }

  driver.acceptOffers(
      {offer.id()},
      {LAUNCH({commandExecutorTask}),
       LAUNCH_GROUP(
           defaultExecutorInfo, createTaskGroupInfo({defaultExecutorTask}))});

  // We track the status updates of each task separately, to verify
  // that they transition from TASK_RUNNING to TASK_FINISHED.
  hashmap<TaskID, TaskState> taskStates;
  taskStates[commandExecutorTask.task_id()] = TASK_STAGING;
  taskStates[defaultExecutorTask.task_id()] = TASK_STAGING;

  foreach (const Future<TaskStatus>& taskStatus, taskStatuses) {
    AWAIT_READY(taskStatus);

    Option<TaskState> taskState = taskStates.get(taskStatus->task_id());
    ASSERT_SOME(taskState);

    switch (taskState.get()) {
      case TASK_STAGING: {
        ASSERT_EQ(TASK_STARTING, taskStatus->state())
          << taskStatus->DebugString();

        taskStates[taskStatus->task_id()] = TASK_STARTING;
        break;
      }
      case TASK_STARTING: {
        ASSERT_EQ(TASK_RUNNING, taskStatus->state())
          << taskStatus->DebugString();

        taskStates[taskStatus->task_id()] = TASK_RUNNING;
        break;
      }
      default: {
        FAIL() << "Unexpected task update: " << taskStatus->DebugString();
        break;
      }
    }
  }

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  // Expect to receive the 'ReregisterSlaveMessage' containing the
  // active frameworks.
  AWAIT_READY(reregisterSlaveMessage);

  // Both tasks should be present; the command executor task shouldn't have an
  // executor ID, but the default executor task should have one.
  EXPECT_EQ(2, reregisterSlaveMessage->tasks().size());
  foreach (const Task& task, reregisterSlaveMessage->tasks()) {
    if (task.task_id() == commandExecutorTask.task_id()) {
      EXPECT_FALSE(task.has_executor_id())
        << "The command executor ID is present, but it"
        << " shouldn't be sent to the master";
    } else {
      EXPECT_TRUE(task.has_executor_id())
        << "The default executor ID is missing";
    }
  }

  AWAIT_READY(slaveReregisteredMessage);

  // Check the response of the master state endpoint.
  Future<process::http::Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> tasks = parse->find<JSON::Array>("frameworks[0].tasks");
  ASSERT_SOME(tasks);
  ASSERT_EQ(2u, tasks->values.size());

  ASSERT_SOME(parse->find<JSON::String>("frameworks[0].tasks[0].id"));

  // Since tasks are stored in a hashmap, there is no strict guarantee of
  // their ordering when listed.
  std::string commandExecutorTaskExecutorId;
  std::string defaultExecutorTaskExecutorId;
  if (parse->find<JSON::String>("frameworks[0].tasks[0].id")->value ==
      commandExecutorTask.task_id().value()) {
    commandExecutorTaskExecutorId =
      parse->find<JSON::String>("frameworks[0].tasks[0].executor_id")->value;
    defaultExecutorTaskExecutorId =
      parse->find<JSON::String>("frameworks[0].tasks[1].executor_id")->value;
  } else {
    defaultExecutorTaskExecutorId =
      parse->find<JSON::String>("frameworks[0].tasks[0].executor_id")->value;
    commandExecutorTaskExecutorId =
      parse->find<JSON::String>("frameworks[0].tasks[1].executor_id")->value;
  }

  // The executor ID of the default executor task should be correct.
  EXPECT_EQ(defaultExecutorInfo.executor_id().value(),
            defaultExecutorTaskExecutorId);

  // The executor ID of the command executor task should be empty.
  EXPECT_EQ("", commandExecutorTaskExecutorId);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
