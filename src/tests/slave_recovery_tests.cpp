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

#include <stdint.h>
#include <unistd.h>

#include <string>

#include <gtest/gtest.h>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/executor.hpp>
#include <mesos/http.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/detector.hpp"
#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "slave/gc.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "messages/messages.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::slave;

using namespace process;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::v1::executor::Call;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class SlaveStateTest : public TemporaryDirectoryTest {};


TEST_F(SlaveStateTest, CheckpointString)
{
  // Checkpoint a test string.
  const string expected = "test";
  const string file = "test-file";
  slave::state::checkpoint(file, expected);

  EXPECT_SOME_EQ(expected, os::read(file));
}


TEST_F(SlaveStateTest, CheckpointProtobufMessage)
{
  // Checkpoint slave id.
  SlaveID expected;
  expected.set_value("slave1");

  const string& file = "slave.id";
  slave::state::checkpoint(file, expected);

  const Result<SlaveID>& actual = ::protobuf::read<SlaveID>(file);
  ASSERT_SOME(actual);

  EXPECT_SOME_EQ(expected, actual);
}


TEST_F(SlaveStateTest, CheckpointRepeatedProtobufMessages)
{
  // Checkpoint resources.
  const Resources expected =
    Resources::parse("cpus:2;mem:512;cpus(role):4;mem(role):1024").get();

  const string file = "resources-file";
  slave::state::checkpoint(file, expected);

  Result<RepeatedPtrField<Resource>> actual =
    ::protobuf::read<RepeatedPtrField<Resource>>(file);

  EXPECT_SOME_EQ(expected, actual);
}


template <typename T>
class SlaveRecoveryTest : public ContainerizerTest<T>
{
public:
  virtual slave::Flags CreateSlaveFlags()
  {
    return ContainerizerTest<T>::CreateSlaveFlags();
  }
};


// Containerizer types to run the tests.
typedef ::testing::Types<slave::MesosContainerizer> ContainerizerTypes;


TYPED_TEST_CASE(SlaveRecoveryTest, ContainerizerTypes);

// Ensure slave recovery works.
TYPED_TEST(SlaveRecoveryTest, RecoverSlaveState)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave> > slave = this->StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  Future<Message> subscribeMessage = FUTURE_CALL_MESSAGE(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  driver.start();

  // Capture the framework pid.
  AWAIT_READY(subscribeMessage);
  UPID frameworkPid = subscribeMessage.get().from;

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  SlaveID slaveId = offers.get()[0].slave_id();

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), Eq(master.get()), _);

  Future<mesos::scheduler::Call> ack = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACKNOWLEDGE, _, _);

  Future<Nothing> _ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the executor pids.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID libprocessPid = registerExecutorMessage.get().from;

  // Capture the update.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update.get().update().status().state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_ack);

  // Recover the state.
  Result<slave::state::State> recover = slave::state::recover(
      paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(recover);
  ASSERT_SOME(recover.get().slave);

  slave::state::SlaveState state = recover.get().slave.get();

  // Check slave id.
  ASSERT_EQ(slaveId, state.id);

  // Check framework id and pid.
  ASSERT_TRUE(state.frameworks.contains(frameworkId));
  ASSERT_SOME_EQ(frameworkPid, state.frameworks[frameworkId].pid);

  ASSERT_TRUE(state.frameworks[frameworkId].executors.contains(executorId));

  // Check executor id and pids.
  const Option<ContainerID>& containerId =
      state.frameworks[frameworkId].executors[executorId].latest;
  ASSERT_SOME(containerId);

  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs.contains(containerId.get()));

  ASSERT_SOME_EQ(
      libprocessPid,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .libprocessPid);


  // Check task id and info.
  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs[containerId.get()]
                .tasks.contains(task.task_id()));

  const Task& t = mesos::internal::protobuf::createTask(
      task, TASK_STAGING, frameworkId);

  ASSERT_SOME_EQ(
      t,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .tasks[task.task_id()]
        .info);

  // Check status update and ack.
  ASSERT_EQ(
      1U,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .tasks[task.task_id()]
        .updates.size());

  ASSERT_EQ(
      update.get().update().uuid(),
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .tasks[task.task_id()]
        .updates.front().uuid());

  const UUID& uuid = UUID::fromBytes(ack.get().acknowledge().uuid());
  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs[containerId.get()]
                .tasks[task.task_id()]
                .acks.contains(uuid));

  // Shut down the executor manually so that it doesn't hang around
  // after the test finishes.
  process::post(libprocessPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();

  this->Shutdown();

  delete containerizer.get();
}


// The slave is killed before the update reaches the scheduler.
// When the slave comes back up it resends the unacknowledged update.
TYPED_TEST(SlaveRecoveryTest, RecoverStatusUpdateManager)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Message expectations.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  // Drop the status update from the slave to the master.
  Future<StatusUpdateMessage> update =
    DROP_PROTOBUF(StatusUpdateMessage(), slave.get(), master.get());

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  // Wait for the status update drop.
  AWAIT_READY(update);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped before the first update for a task is received
// from the HTTP based executor. When it comes back up with recovery=reconnect,
// make sure the executor subscribes and the slave properly sends the update.
TYPED_TEST(SlaveRecoveryTest, ReconnectHTTPExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const std::string id("slave");

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), id, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // TODO(anand): Use the HTTP based command executor once MESOS-3558 is
  // resolved.
  ExecutorInfo executorInfo =
    CREATE_EXECUTOR_INFO(
        "http",
        path::join(tests::flags.build_dir, "src", "test-http-executor"));

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(executorInfo);

  Future<v1::executor::Call> updateCall1 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  Future<v1::executor::Call> updateCall2 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status updates are received.
  AWAIT_READY(updateCall1);
  AWAIT_READY(updateCall2);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<v1::executor::Call> subscribeCall =
    FUTURE_HTTP_CALL(Call(), Call::SUBSCRIBE, _, ContentType::PROTOBUF);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), id, flags);
  ASSERT_SOME(slave);

  // Ensure that the executor subscribes again.
  AWAIT_READY(subscribeCall);

  ASSERT_EQ(2, subscribeCall.get().subscribe().unacknowledged_updates().size());
  ASSERT_EQ(1, subscribeCall.get().subscribe().unacknowledged_tasks().size());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up with recovery=reconnect, make
// sure the executor re-registers and the slave properly sends the update.
TYPED_TEST(SlaveRecoveryTest, ReconnectExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status update is received.
  AWAIT_READY(statusUpdate);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped before the (command) executor is registered.
// When it comes back up with recovery=reconnect, make sure the
// executor is killed and the task is transitioned to LOST.
TYPED_TEST(SlaveRecoveryTest, RecoverUnregisteredExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  // Drop the executor registration message.
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Stop the slave before the executor is registered.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT,
            status->reason());

  // Master should subsequently reoffer the same resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped after a non-terminal update is received.
// The command executor terminates when the slave is down.
// When it comes back up with recovery=reconnect, make
// sure the task is properly transitioned to LOST.
TYPED_TEST(SlaveRecoveryTest, RecoverTerminatedExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Now shut down the executor, when the slave is down.
  process::post(executorPid, ShutdownExecutorMessage());

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT,
            status->reason());

  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Master should subsequently reoffer the same resources.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped after a non-terminal update is received.
// The command executor is expected to self-terminate while the slave
// is down, because the recovery timeout elapses.
// When the slave comes back up with recovery=reconnect, make
// sure the task is properly transitioned to FAILED.
// TODO(bmahler): Disabled for MESOS-685: the exited() event for the
// slave will not be delivered to the executor driver.
TYPED_TEST(SlaveRecoveryTest, DISABLED_RecoveryTimeout)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  // Set a short recovery timeout, as we can't control the executor
  // driver time when using the process / cgroups isolators.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.recovery_timeout = Milliseconds(1);

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure the executor terminates by causing the recovery timeout
  // to elapse while disconnected from the slave.
  os::sleep(Milliseconds(1));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped after an executor is completed (i.e., it has
// terminated and all its updates have been acknowledged).
// When it comes back up with recovery=reconnect, make
// sure the recovery successfully completes.
TYPED_TEST(SlaveRecoveryTest, RecoverCompletedExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "exit 0");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2); // TASK_RUNNING and TASK_FINISHED updates.

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  Future<Nothing> schedule = FUTURE_DISPATCH(
      _, &GarbageCollectorProcess::schedule);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // We use 'gc.schedule' as a proxy for the cleanup of the executor.
  AWAIT_READY(schedule);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<Nothing> schedule2 = FUTURE_DISPATCH(
      _, &GarbageCollectorProcess::schedule);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // We use 'gc.schedule' as a proxy for the cleanup of the executor.
  AWAIT_READY(schedule2);

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped before a terminal update is received from the HTTP
// based executor. The slave is then restarted in recovery=cleanup mode.
// It kills the executor, and terminates. Master should then send TASK_LOST.
TYPED_TEST(SlaveRecoveryTest, CleanupHTTPExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const std::string id("slave");

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), id, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // TODO(anand): Use the HTTP based command executor once MESOS-3558 is
  // resolved.
  ExecutorInfo executorInfo =
    CREATE_EXECUTOR_INFO(
        "http",
        path::join(tests::flags.build_dir, "src", "test-http-executor"));

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(executorInfo);

  Future<v1::executor::Call> updateCall1 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  Future<v1::executor::Call> updateCall2 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status updates are received.
  AWAIT_READY(updateCall1);
  AWAIT_READY(updateCall2);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Slave in cleanup mode shouldn't re-register with the master and
  // hence no offers should be made by the master.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .Times(0);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  // Restart the slave in 'cleanup' recovery mode with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  flags.recover = "cleanup";
  slave = this->StartSlave(containerizer2.get(), id, flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Wait for recovery to complete.
  AWAIT_READY(__recover);

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is stopped after a non-terminal update is received.
// Slave is restarted in recovery=cleanup mode. It kills the command
// executor, and terminates. Master should then send TASK_LOST.
TYPED_TEST(SlaveRecoveryTest, CleanupExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Slave in cleanup mode shouldn't re-register with the master and
  // hence no offers should be made by the master.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .Times(0);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  // Restart the slave in 'cleanup' recovery mode with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  flags.recover = "cleanup";
  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Wait for recovery to complete.
  AWAIT_READY(__recover);

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// This test checks whether a non-checkpointing framework is
// properly removed, when a slave is disconnected.
TYPED_TEST(SlaveRecoveryTest, RemoveNonCheckpointingFramework)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave> > slave = this->StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch 2 tasks from this offer.
  vector<TaskInfo> tasks;
  Offer offer = offers.get()[0];

  Offer offer1 = offer;
  offer1.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:512").get());
  tasks.push_back(createTask(offer1, "sleep 1000")); // Long-running task.

  Offer offer2 = offer;
  offer2.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:512").get());
  tasks.push_back(createTask(offer2, "sleep 1000")); // Long-running task,

  ASSERT_TRUE(Resources(offer.resources()).contains(
        Resources(offer1.resources()) +
        Resources(offer2.resources())));

  Future<Nothing> update1;
  Future<Nothing> update2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update1))
    .WillOnce(FutureSatisfy(&update2));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait for TASK_RUNNING updates from the tasks.
  AWAIT_READY(update1);
  AWAIT_READY(update2);

  // The master should generate TASK_LOST updates once the slave is stopped.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  this->Stop(slave.get());

  // Scheduler should receive the TASK_LOST updates.
  AWAIT_READY(status1);
  ASSERT_EQ(TASK_LOST, status1.get().state());

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_LOST, status2.get().state());

  driver.stop();
  driver.join();

  // Destroy all the containers before we destroy the containerizer. We need to
  // do this manually because there are no slaves left in the cluster.
  Future<hashset<ContainerID> > containers = containerizer.get()->containers();
  AWAIT_READY(containers);

  foreach (const ContainerID& containerId, containers.get()) {
    Future<containerizer::Termination> wait =
      containerizer.get()->wait(containerId);

    containerizer.get()->destroy(containerId);
    AWAIT_READY(wait);
  }

  delete containerizer.get();

  this->Shutdown();
}


// This test ensures that no checkpointing happens for a
// framework that has disabled checkpointing.
TYPED_TEST(SlaveRecoveryTest, NonCheckpointingFramework)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave> > slave = this->StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(false);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<Nothing> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for TASK_RUNNING update.
  AWAIT_READY(update);

  Clock::pause();

  Future<Nothing> updateFramework = FUTURE_DISPATCH(_, &Slave::updateFramework);

  // Simulate a 'UpdateFrameworkMessage' to ensure framework pid is
  // not being checkpointed.
  process::dispatch(slave.get(), &Slave::updateFramework, frameworkId, "");

  AWAIT_READY(updateFramework);

  Clock::settle(); // Wait for the slave to act on the dispatch.

  // Ensure that the framework info is not being checkpointed.
  const string& path = paths::getFrameworkPath(
      paths::getMetaRootDir(flags.work_dir),
      task.slave_id(),
      frameworkId);

  ASSERT_FALSE(os::exists(path));

  Clock::resume();

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer.get();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. This test ensures that a
// restarted slave is able to communicate with all components
// (scheduler, master, executor).
TYPED_TEST(SlaveRecoveryTest, KillTask)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to re-register.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_KILLED, status.get().state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// When the slave is down we modify the BOOT_ID_FILE to simulate a
// reboot. The subsequent run of the slave should not recover.
TYPED_TEST(SlaveRecoveryTest, Reboot)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.strict = false;

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor ID and PID.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage.get().from;

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  // Capture the container ID.
  Future<hashset<ContainerID> > containers =
    containerizer1.get()->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *containers.get().begin();

  this->Stop(slave.get());
  delete containerizer1.get();

  // Get the executor's pid so we can reap it to properly simulate a
  // reboot.
  string pidPath = paths::getForkedPidPath(
        paths::getMetaRootDir(flags.work_dir),
        slaveId,
        frameworkId,
        executorId,
        containerId);

  Try<string> read = os::read(pidPath);
  ASSERT_SOME(read);

  Try<pid_t> pid = numify<pid_t>(read.get());
  ASSERT_SOME(pid);

  Future<Option<int> > executorStatus = process::reap(pid.get());

  // Shut down the executor manually and wait until it's been reaped.
  process::post(executorPid, ShutdownExecutorMessage());

  AWAIT_READY(executorStatus);

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      paths::getBootIdPath(paths::getMetaRootDir(flags.work_dir)),
      "rebooted! ;)"));

  Future<RegisterSlaveMessage> registerSlave =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlave);

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// When the slave is down we remove the "latest" symlink in the
// executor's run directory, to simulate a situation where the
// recovered slave (--no-strict) cannot recover the executor and
// hence schedules it for gc.
TYPED_TEST(SlaveRecoveryTest, GCExecutor)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.strict = false;

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor id and pid.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage.get().from;

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  this->Stop(slave.get());

  // Destroy all the containers before we destroy the containerizer.
  Future<hashset<ContainerID> > containers = containerizer1.get()->containers();
  AWAIT_READY(containers);

  foreach (const ContainerID& containerId, containers.get()) {
    Future<containerizer::Termination> wait =
      containerizer1.get()->wait(containerId);

    containerizer1.get()->destroy(containerId);
    AWAIT_READY(wait);
  }

  delete containerizer1.get();

  // Remove the symlink "latest" in the executor directory
  // to simulate a non-recoverable executor.
  ASSERT_SOME(os::rm(paths::getExecutorLatestRunPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId,
      frameworkId,
      executorId)));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);

  AWAIT_READY(slaveReregisteredMessage);

  Clock::advance(flags.gc_delay);

  Clock::settle();

  // Executor's work and meta directories should be gc'ed by now.
  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      flags.work_dir, slaveId, frameworkId, executorId)));

  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId,
      frameworkId,
      executorId)));

  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave is asked to shutdown. When it comes back up, it should
// register as a new slave.
TYPED_TEST(SlaveRecoveryTest, ShutdownSlave)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))  // Initial offer.
    .WillOnce(FutureArg<1>(&offers2)); // Task resources re-offered.

  driver.start();

  AWAIT_READY(offers1);

  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  Future<Nothing> statusUpdate1;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&statusUpdate1))
    .WillOnce(Return());  // Ignore TASK_FAILED update.

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor.get().from;

  AWAIT_READY(statusUpdate1); // Wait for TASK_RUNNING update.

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // We shut down the executor here so that a shutting down slave
  // does not spend too much time waiting for the executor to exit.
  process::post(executorPid, ShutdownExecutorMessage());

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (executorTerminated.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_READY(executorTerminated);
  AWAIT_READY(offers2);

  Clock::resume();

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  this->Stop(slave.get(), true); // Send a "shut down".
  delete containerizer1.get();

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Now restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Ensure that the slave registered with a new id.
  AWAIT_READY(offers3);

  EXPECT_NE(0u, offers3.get().size());
  // Make sure all slave resources are reoffered.
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers3.get()[0].resources()));

  // Ensure the slave id is different.
  ASSERT_NE(
      offers1.get()[0].slave_id().value(), offers3.get()[0].slave_id().value());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The slave should shutdown when it receives a SIGUSR1 signal.
TYPED_TEST(SlaveRecoveryTest, ShutdownSlaveSIGUSR1)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave> > slave = this->StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))  // Initial offer.
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(_, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<Nothing> signaled =
    FUTURE_DISPATCH(_, &Slave::signaled);

  Future<UnregisterSlaveMessage> unregisterSlaveMessage =
    FUTURE_PROTOBUF(UnregisterSlaveMessage(), slave.get(), master.get());

  // Send SIGUSR1 signal to the slave.
  kill(getpid(), SIGUSR1);

  AWAIT_READY(signaled);
  AWAIT_READY(unregisterSlaveMessage);
  AWAIT_READY(executorTerminated);

  // The master should send a TASK_LOST and slaveLost.
  AWAIT_READY(status2);
  ASSERT_EQ(TASK_LOST, status2.get().state());
  AWAIT_READY(slaveLost);

  // Make sure the slave terminates.
  ASSERT_TRUE(process::wait(slave.get(), Seconds(10)));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer.get();
}


// The slave fails to do recovery and tries to register as a new slave. The
// master should give it a new id and transition all the tasks of the old slave
// to LOST.
TYPED_TEST(SlaveRecoveryTest, RegisterDisconnectedSlave)
{
  master::Flags masterFlags = this->CreateMasterFlags();

  // Disable authentication so the spoofed re-registration below works.
  masterFlags.authenticate_slaves = false;

  Try<PID<Master> > master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave> > slave = this->StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Capture the slave and framework ids.
  SlaveID slaveId = offers.get()[0].slave_id();
  FrameworkID frameworkId = offers.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the executor pid.
  AWAIT_READY(registerExecutorMessage);
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage.get().body);
  UPID executorPid = registerExecutorMessage.get().from;

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  this->Stop(slave.get());

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status2));

  // Spoof the registration attempt of a slave that failed recovery.
  // We do this because simply restarting the slave will result in a slave
  // with a different pid than the previous one.
  post(slave.get(), master.get(), registerSlaveMessage.get());

  // Scheduler should get a TASK_LOST message.
  AWAIT_READY(status2);
  ASSERT_EQ(TASK_LOST, status2.get().state());

  driver.stop();
  driver.join();

  // Destroy all the containers before we destroy the containerizer. We need to
  // do this manually because there are no slaves left in the cluster.
  Future<hashset<ContainerID> > containers = containerizer.get()->containers();
  AWAIT_READY(containers);

  foreach (const ContainerID& containerId, containers.get()) {
    Future<containerizer::Termination> wait =
      containerizer.get()->wait(containerId);

    containerizer.get()->destroy(containerId);
    AWAIT_READY(wait);
  }

  delete containerizer.get();

  this->Shutdown();
}


// This test verifies that a KillTask message received by the master when a
// slave is disconnected is properly reconciled when the slave reregisters.
TYPED_TEST(SlaveRecoveryTest, ReconcileKillTask)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
      FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  // Expecting TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for TASK_RUNNING update to be acknowledged.
  AWAIT_READY(_statusUpdateAcknowledgement);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Now send a KillTask message to the master. This will not be
  // received by the slave because it is down.
  driver.killTask(task.task_id());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Now restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Scheduler should get a TASK_KILLED message.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_KILLED, status.get().state());

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// This test verifies that when the slave recovers and re-registers
// with a framework that was shutdown when the slave was down, it gets
// a ShutdownFramework message.
TYPED_TEST(SlaveRecoveryTest, ReconcileShutdownFramework)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Capture the slave and framework ids.
  SlaveID slaveId = offers.get()[0].slave_id();
  FrameworkID frameworkId = offers.get()[0].framework_id();

  // Expecting TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for TASK_RUNNING update to be acknowledged.
  AWAIT_READY(_statusUpdateAcknowledgement);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  // Now stop the framework.
  driver.stop();
  driver.join();

  // Wait until the framework is removed.
  AWAIT_READY(teardownCall);

  Future<ShutdownFrameworkMessage> shutdownFrameworkMessage =
    FUTURE_PROTOBUF(ShutdownFrameworkMessage(), _, _);

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // Now restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Slave should get a ShutdownFrameworkMessage.
  AWAIT_READY(shutdownFrameworkMessage);

  // Ensure that the executor is terminated.
  AWAIT_READY(executorTerminated);

  this->Shutdown();
  delete containerizer2.get();
}


// This ensures that reconciliation properly deals with tasks
// present in the master and missing from the slave. Notably:
//   1. The tasks are sent to LOST.
//   2. The task resources are recovered.
// TODO(bmahler): Ensure the executor resources are recovered by
// using an explicit executor.
TYPED_TEST(SlaveRecoveryTest, ReconcileTasksMissingFromSlave)
{
  TestAllocator<master::allocator::HierarchicalDRFAllocator> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master> > master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _));

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _));

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Start a task on the slave so that the master has knowledge of it.
  // We'll ensure the slave does not have this task when it
  // re-registers by wiping the relevant meta directory.
  TaskInfo task = createTask(offers1.get()[0], "sleep 10");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  EXPECT_CALL(allocator, deactivateSlave(_));

  this->Stop(slave.get());
  delete containerizer1.get();

  // Construct the framework meta directory that needs wiping.
  string frameworkPath = paths::getFrameworkPath(
      paths::getMetaRootDir(flags.work_dir),
      offers1.get()[0].slave_id(),
      frameworkId.get());

  // Kill the forked pid, so that we don't leak a child process.
  // Construct the executor id from the task id, since this test
  // uses a command executor.
  Result<slave::state::State> state =
    slave::state::recover(slave::paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(state);
  ASSERT_SOME(state.get().slave);
  ASSERT_TRUE(state.get().slave.get().frameworks.contains(frameworkId.get()));

  slave::state::FrameworkState frameworkState =
    state.get().slave.get().frameworks.get(frameworkId.get()).get();

  ASSERT_EQ(1u, frameworkState.executors.size());

  slave::state::ExecutorState executorState =
    frameworkState.executors.begin()->second;

  ASSERT_EQ(1u, executorState.runs.size());

  slave::state::RunState runState = executorState.runs.begin()->second;

  ASSERT_SOME(runState.forkedPid);

  ASSERT_SOME(os::killtree(runState.forkedPid.get(), SIGKILL));

  // Remove the framework meta directory, so that the slave will not
  // recover the task.
  ASSERT_SOME(os::rmdir(frameworkPath, true));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(allocator, activateSlave(_));
  EXPECT_CALL(allocator, recoverResources(_, _, _, _));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  // Wait for TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status.get().state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  // If there was an outstanding offer, we can get a call to
  // recoverResources when we stop the scheduler.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(Return());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. A scheduler failover happens
// when the slave is down. This test verifies that a scheduler
// failover will not affect the slave recovery process.
TYPED_TEST(SlaveRecoveryTest, SchedulerFailover)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;

  FrameworkInfo framework1 = DEFAULT_FRAMEWORK_INFO;
  framework1.set_checkpoint(true);

  MesosSchedulerDriver driver1(
      &sched1, framework1, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Create a long running task.
  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  EXPECT_CALL(sched1, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());
  framework2.set_checkpoint(true);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);
  AWAIT_READY(sched1Error);

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to re-register.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched2, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Kill the task.
  driver2.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_KILLED, status.get().state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver2.stop();
  driver2.join();

  driver1.stop();
  driver1.join();

  this->Shutdown();
  delete containerizer2.get();
}


// The purpose of this test is to ensure that during a network
// partition, the master will remove a partitioned slave. When the
// partition is removed, the slave will receive a ShutdownMessage.
// When the slave starts again on the same host, we verify that the
// slave will not try to reregister itself with the master. It will
// register itself with the master and get a new slave id.
TYPED_TEST(SlaveRecoveryTest, PartitionedSlave)
{
  master::Flags masterFlags = this->CreateMasterFlags();
  Try<PID<Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_MESSAGES(Eq(PongSlaveMessage().GetTypeName()), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  // Long running task.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), _, slave.get());

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_slave_ping_timeouts) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.slave_ping_timeout);
    Clock::settle();
  }

  Clock::advance(masterFlags.slave_ping_timeout);
  Clock::settle();

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  // The master will have notified the framework of the lost task.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // Wait for the master to attempt to shut down the slave.
  AWAIT_READY(shutdownMessage);

  // Wait for the executor to be terminated.
  while (executorTerminated.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_READY(executorTerminated);
  Clock::settle();

  Clock::resume();

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// This test verifies that if the master changes when the slave is
// down, the slave can still recover the task when it restarts. We
// verify its correctness by killing the task from the scheduler.
TYPED_TEST(SlaveRecoveryTest, MasterFailover)
{
  // Step 1. Run a task.
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()));
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), frameworkInfo);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Step 2. Simulate failed over master by restarting the master.
  this->Stop(master.get());
  master = this->StartMaster();
  ASSERT_SOME(master);

  EXPECT_CALL(sched, disconnected(_));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Simulate a new master detected event to the scheduler.
  detector->appoint(master.get());

  // Framework should get a registered callback.
  AWAIT_READY(registered);

  // Step 3. Restart the slave and kill the task.
  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to re-register.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Wait for the executor to terminate before shutting down the
  // slave in order to give cgroups (if applicable) time to clean up.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_KILLED, status.get().state());

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  ASSERT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  AWAIT_READY(executorTerminated);

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// In this test there are two frameworks and one slave. Each
// framework launches a task before the slave goes down. We verify
// that the two frameworks and their tasks are recovered after the
// slave restarts.
TYPED_TEST(SlaveRecoveryTest, MultipleFrameworks)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  // Framework 1.
  MockScheduler sched1;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_checkpoint(true);

  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Use part of the resources in the offer so that the rest can be
  // offered to framework 2.
  Offer offer1 = offers1.get()[0];
  offer1.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:512").get());

  // Framework 1 launches a task.
  TaskInfo task1 = createTask(offer1, "sleep 1000");

  EXPECT_CALL(sched1, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offer1.id(), {task1});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);

  // Framework 2.
  MockScheduler sched2;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_checkpoint(true);

  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver2.start();

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Framework 2 launches a task.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  EXPECT_CALL(sched2, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);
  driver2.launchTasks(offers2.get()[0].id(), {task2});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement2);

  this->Stop(slave.get());
  delete containerizer1.get();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage2 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);
  Future<ReregisterExecutorMessage> reregisterExecutorMessage1 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executors to re-register.
  AWAIT_READY(reregisterExecutorMessage1);
  AWAIT_READY(reregisterExecutorMessage2);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  // Expectations for the status changes as a result of killing the
  // tasks.
  Future<TaskStatus> status1;
  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<TaskStatus> status2;
  EXPECT_CALL(sched2, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  // Wait for the executors to terminate before shutting down the
  // slave in order to give cgroups (if applicable) time to clean up.
  Future<Nothing> executorTerminated1 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);
  Future<Nothing> executorTerminated2 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // Kill task 1.
  driver1.killTask(task1.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status1);
  ASSERT_EQ(TASK_KILLED, status1.get().state());

  // Kill task 2.
  driver2.killTask(task2.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status2);
  ASSERT_EQ(TASK_KILLED, status2.get().state());

  AWAIT_READY(executorTerminated1);
  AWAIT_READY(executorTerminated2);

  driver1.stop();
  driver1.join();
  driver2.stop();
  driver2.join();

  this->Shutdown();
  delete containerizer2.get();
}


// This test verifies that slave recovery works properly even if
// multiple slaves are co-located on the same host.
TYPED_TEST(SlaveRecoveryTest, MultipleSlaves)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  driver.start();

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Start the first slave.
  slave::Flags flags1 = this->CreateSlaveFlags();

  // NOTE: We cannot run multiple slaves simultaneously on a host if
  // cgroups isolation is involved.
  flags1.isolation = "filesystem/posix,posix/mem,posix/cpu";

#ifdef __linux__
  // Disable putting slave into cgroup(s) because this is a multi-slave test.
  flags1.slave_subsystems = None();
#endif

  Fetcher fetcher;

  Try<TypeParam*> containerizer1 = TypeParam::create(flags1, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave1 = this->StartSlave(containerizer1.get(), flags1);
  ASSERT_SOME(slave1);

  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1.get().size());

  // Launch a long running task in the first slave.
  TaskInfo task1 = createTask(offers1.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(1);

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(slave1.get(), &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // Start the second slave.
  slave::Flags flags2 = this->CreateSlaveFlags();

  // NOTE: We cannot run multiple slaves simultaneously on a host if
  // cgroups isolation is involved.
  flags2.isolation = "filesystem/posix,posix/mem,posix/cpu";

#ifdef __linux__
  // Disable putting slave into cgroup(s) because this is a multi-slave test.
  flags2.slave_subsystems = None();
#endif

  Try<TypeParam*> containerizer2 = TypeParam::create(flags2, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Try<PID<Slave> > slave2 = this->StartSlave(containerizer2.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2.get().size());

  // Launch a long running task in each slave.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(1);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave2.get(), &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers2.get()[0].id(), {task2});

  // Wait for the ACKs to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement2);

  this->Stop(slave1.get());
  delete containerizer1.get();
  this->Stop(slave2.get());
  delete containerizer2.get();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage2 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);
  Future<ReregisterExecutorMessage> reregisterExecutorMessage1 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage2 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);
  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart both slaves using the same flags with new containerizers.
  Try<TypeParam*> containerizer3 = TypeParam::create(flags1, true, &fetcher);
  ASSERT_SOME(containerizer3);

  Clock::pause();

  slave1 = this->StartSlave(containerizer3.get(), flags1);
  ASSERT_SOME(slave1);

  Try<TypeParam*> containerizer4 = TypeParam::create(flags2, true, &fetcher);
  ASSERT_SOME(containerizer4);

  slave2 = this->StartSlave(containerizer4.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(reregisterExecutorMessage1);
  AWAIT_READY(reregisterExecutorMessage2);

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slaves to re-register.
  AWAIT_READY(slaveReregisteredMessage1);
  AWAIT_READY(slaveReregisteredMessage2);

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  Future<Nothing> executorTerminated2 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);
  Future<Nothing> executorTerminated1 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  // Kill both tasks.
  driver.killTask(task1.task_id());
  driver.killTask(task2.task_id());

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_KILLED, status1.get().state());

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_KILLED, status2.get().state());

  AWAIT_READY(executorTerminated1);
  AWAIT_READY(executorTerminated2);

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer3.get();
  delete containerizer4.get();
}


// The slave is stopped after it dispatched Containerizer::launch but
// before the containerizer has processed the launch. When the slave
// comes back up it should send a TASK_FAILED for the task.
// NOTE: This is a 'TYPED_TEST' but we don't use 'TypeParam'.
TYPED_TEST(SlaveRecoveryTest, RestartBeforeContainerizerLaunch)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  TestContainerizer* containerizer1 = new TestContainerizer();

  Try<PID<Slave> > slave = this->StartSlave(containerizer1, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Expect the launch but don't do anything.
  Future<Nothing> launch;
  EXPECT_CALL(*containerizer1, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&launch),
                    Return(Future<bool>())));

  // No status update should be sent for now.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(0);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Once we see the call to launch, restart the slave.
  AWAIT_READY(launch);

  this->Stop(slave.get());
  delete containerizer1;

  Future<TaskStatus> status;
  // There is a race here where the Slave may reregister before we
  // shut down. If it does, it causes the StatusUpdateManager to
  // flush which will cause a duplicate status update to be sent. As
  // such, we ignore any subsequent updates.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  TestContainerizer containerizer2;

  Clock::pause();

  slave = this->StartSlave(&containerizer2, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED,
            status->reason());

  driver.stop();
  driver.join();

  this->Shutdown();
}


// We explicitly instantiate a SlaveRecoveryTest for test cases where
// we assume we'll only have the MesosContainerizer.
class MesosContainerizerSlaveRecoveryTest
  : public SlaveRecoveryTest<MesosContainerizer> {};


TEST_F(MesosContainerizerSlaveRecoveryTest, ResourceStatistics)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  // Message expectations.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutor);

  this->Stop(slave.get());
  delete containerizer1.get();

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has re-registered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  // Restart the slave (use same flags) with a new containerizer.
  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  Future<hashset<ContainerID> > containers = containerizer2.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  Future<ResourceStatistics> usage = containerizer2.get()->usage(containerId);
  AWAIT_READY(usage);

  // Check the resource limits are set.
  EXPECT_TRUE(usage.get().has_cpus_limit());
  EXPECT_TRUE(usage.get().has_mem_limit_bytes());

  Future<containerizer::Termination> wait =
    containerizer2.get()->wait(containerId);

  containerizer2.get()->destroy(containerId);

  AWAIT_READY(wait);

  driver.stop();
  driver.join();

  this->Shutdown();

  delete containerizer2.get();
}


#ifdef __linux__
// Test that the perf event isolator can be enabled on a new slave.
// Previously created containers will not report perf statistics but
// newly created containers will.
TEST_F(MesosContainerizerSlaveRecoveryTest, CGROUPS_ROOT_PerfRollForward)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave using a containerizer without a perf event
  // isolator.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";
  flags.slave_subsystems = "";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  SlaveID slaveId = offers1.get()[0].slave_id();

  TaskInfo task1 = createTask(
      slaveId, Resources::parse("cpus:0.5;mem:128").get(), "sleep 1000");

  // Message expectations.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(registerExecutor);

  Future<hashset<ContainerID> > containers = containerizer1.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId1 = *(containers.get().begin());

  Future<ResourceStatistics> usage = containerizer1.get()->usage(containerId1);
  AWAIT_READY(usage);

  // There should not be any perf statistics.
  EXPECT_FALSE(usage.get().has_perf());

  this->Stop(slave.get());
  delete containerizer1.get();

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has re-registered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  // Start a slave using a containerizer with a perf event isolator.
  flags.isolation = "cgroups/cpu,cgroups/mem,cgroups/perf_event";
  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);

  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  // The first container should not report perf statistics.
  usage = containerizer2.get()->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_FALSE(usage.get().has_perf());

  // Start a new container which will start reporting perf statistics.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  // Message expectations.
  registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(registerExecutor);

  containers = containerizer2.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(2u, containers.get().size());
  EXPECT_TRUE(containers.get().contains(containerId1));

  ContainerID containerId2;
  foreach (const ContainerID containerId, containers.get()) {
    if (containerId != containerId1) {
      containerId2.CopyFrom(containerId);
    }
  }

  usage = containerizer2.get()->usage(containerId2);
  AWAIT_READY(usage);

  EXPECT_TRUE(usage.get().has_perf());

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// Test that a container started without namespace/pid isolation can
// be destroyed correctly with namespace/pid isolation enabled.
TEST_F(MesosContainerizerSlaveRecoveryTest, CGROUPS_ROOT_PidNamespaceForward)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave using a containerizer without pid namespace
  // isolation.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";
  flags.slave_subsystems = "";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  SlaveID slaveId = offers1.get()[0].slave_id();

  TaskInfo task1 = createTask(
      slaveId, Resources::parse("cpus:0.5;mem:128").get(), "sleep 1000");

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(registerExecutorMessage);

  Future<hashset<ContainerID> > containers = containerizer1.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  // Stop the slave.
  this->Stop(slave.get());
  delete containerizer1.get();

  // Start a slave using a containerizer with pid namespace isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem,namespaces/pid";

  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Set up to wait on the container's termination.
  Future<containerizer::Termination> termination =
    containerizer2.get()->wait(containerId);

  // Destroy the container.
  containerizer2.get()->destroy(containerId);

  AWAIT_READY(termination);

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}


// Test that a container started with namespace/pid isolation can
// be destroyed correctly without namespace/pid isolation enabled.
TEST_F(MesosContainerizerSlaveRecoveryTest, CGROUPS_ROOT_PidNamespaceBackward)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave using a containerizer with pid namespace isolation.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem,namespaces/pid";
  flags.slave_subsystems = "";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = this->StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  SlaveID slaveId = offers1.get()[0].slave_id();

  TaskInfo task1 = createTask(
      slaveId, Resources::parse("cpus:0.5;mem:128").get(), "sleep 1000");

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(registerExecutorMessage);

  Future<hashset<ContainerID> > containers = containerizer1.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  // Stop the slave.
  this->Stop(slave.get());
  delete containerizer1.get();

  // Start a slave using a containerizer without pid namespace
  // isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem";

  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  slave = this->StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Set up to wait on the container's termination.
  Future<containerizer::Termination> termination =
    containerizer2.get()->wait(containerId);

  // Destroy the container.
  containerizer2.get()->destroy(containerId);

  AWAIT_READY(termination);

  driver.stop();
  driver.join();

  this->Shutdown();
  delete containerizer2.get();
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
