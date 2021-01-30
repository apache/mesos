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

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <string>

#include <gtest/gtest.h>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/executor.hpp>
#include <mesos/http.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include <stout/os/killtree.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/detector/standalone.hpp"

#include "slave/gc.hpp"
#include "slave/gc_process.hpp"
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
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::slave;

using namespace process;

using process::http::OK;
using process::http::Response;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::slave::ContainerTermination;

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

  EXPECT_SOME_EQ(expected, slave::state::read<string>(file));
}


TEST_F(SlaveStateTest, CheckpointProtobufMessage)
{
  // Checkpoint slave id.
  SlaveID expected;
  expected.set_value("agent1");

  const string file = "slave.id";
  slave::state::checkpoint(file, expected);

  const Result<SlaveID> actual = slave::state::read<SlaveID>(file);
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

  const Result<Resources> actual = slave::state::read<Resources>(file);

  ASSERT_SOME(actual);
  EXPECT_SOME_EQ(expected, actual);
}


template <typename T>
class SlaveRecoveryTest : public ContainerizerTest<T>
{
public:
  slave::Flags CreateSlaveFlags() override
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
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  Future<Message> subscribeMessage = FUTURE_CALL_MESSAGE(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  driver.start();

  // Capture the framework pid.
  AWAIT_READY(subscribeMessage);
  UPID frameworkPid = subscribeMessage->from;

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  SlaveID slaveId = offers.get()[0].slave_id();

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Scheduler expectations.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), Eq(master.get()->pid), _);

  Future<mesos::scheduler::Call> ack = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACKNOWLEDGE, _, _);

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the executor pids.
  AWAIT_READY(registerExecutorMessage);

  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID libprocessPid = registerExecutorMessage->from;

  // Capture the update.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_STARTING, update->update().status().state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  // Recover the state.
  Result<slave::state::State> recover = slave::state::recover(
      paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(recover);
  ASSERT_SOME(recover->slave);

  slave::state::SlaveState state = recover->slave.get();

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
  // (the number might be bigger than 1 because we might have
  // received any number of additional TASK_RUNNING updates)
  ASSERT_LE(
      1U,
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .tasks[task.task_id()]
        .updates.size());

  ASSERT_EQ(
      update->update().uuid(),
      state
        .frameworks[frameworkId]
        .executors[executorId]
        .runs[containerId.get()]
        .tasks[task.task_id()]
        .updates.front().uuid());

  const id::UUID uuid = id::UUID::fromBytes(ack->acknowledge().uuid()).get();
  ASSERT_TRUE(state
                .frameworks[frameworkId]
                .executors[executorId]
                .runs[containerId.get()]
                .tasks[task.task_id()]
                .acks.contains(uuid));

  // Shut down the executor manually so that it doesn't hang around
  // after the test finishes.
  process::post(slave.get()->pid, libprocessPid, ShutdownExecutorMessage());

  driver.stop();
  driver.join();
}


// The slave is killed before the update reaches the scheduler.
// When the slave comes back up it resends the unacknowledged update.
TYPED_TEST(SlaveRecoveryTest, RecoverTaskStatusUpdateManager)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Message expectations.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  // Drop the status update from the slave to the master.
  Future<StatusUpdateMessage> update =
    DROP_PROTOBUF(StatusUpdateMessage(), slave.get()->pid, master.get()->pid);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutor);

  // Wait for the status update drop.
  AWAIT_READY(update);

  slave.get()->terminate();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_STARTING, status->state());

  driver.stop();
  driver.join();
}


// The slave is stopped before the first update for a task is received from the
// HTTP based command executor. When it comes back up with recovery=reconnect,
// make sure the executor subscribes and the slave properly sends the update.
//
// TODO(alexr): Enable after MESOS-6623 is resolved.
TYPED_TEST(SlaveRecoveryTest, DISABLED_ReconnectHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  // Launch a task with the HTTP based command executor.
  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  Future<v1::executor::Call> updateCall =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status updates are received.
  AWAIT_READY(updateCall);

  slave.get()->terminate();

  Future<v1::executor::Call> subscribeCall =
    FUTURE_HTTP_CALL(Call(), Call::SUBSCRIBE, _, ContentType::PROTOBUF);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Ensure that the executor subscribes again.
  AWAIT_READY(subscribeCall);

  EXPECT_EQ(1, subscribeCall->subscribe().unacknowledged_updates().size());
  EXPECT_EQ(1, subscribeCall->subscribe().unacknowledged_tasks().size());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_STARTING, status->state());

  driver.stop();
  driver.join();
}


// The agent is stopped after dropping the updates for all tasks in the
// task group from the default executor. When it comes back up with
// recovery=reconnect, make sure the executor subscribes and the agent
// properly sends the updates.
//
// TODO(anand): Remove the `ROOT_CGROUPS` prefix once the posix isolator
// is nested aware.
//
// TODO(alexr): Enable after MESOS-6623 is resolved.
TYPED_TEST(SlaveRecoveryTest, DISABLED_ROOT_CGROUPS_ReconnectDefaultExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // Disable AuthN on the agent.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  mesos.send(v1::createCallSubscribe(evolve(frameworkInfo)));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, SLEEP_COMMAND(1000)));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, SLEEP_COMMAND(1000)));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  {
    v1::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(v1::scheduler::Call::ACCEPT);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  Future<v1::executor::Call> updateCall1 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  Future<v1::executor::Call> updateCall2 =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  Future<v1::agent::Call> waitCall1 = FUTURE_HTTP_CALL(
      v1::agent::Call(),
      v1::agent::Call::WAIT_NESTED_CONTAINER,
      _,
      ContentType::PROTOBUF);

  Future<v1::agent::Call> waitCall2 = FUTURE_HTTP_CALL(
      v1::agent::Call(),
      v1::agent::Call::WAIT_NESTED_CONTAINER,
      _,
      ContentType::PROTOBUF);

  // Stop the agent after dropping the update calls and upon receiving the
  // wait calls. We can't drop the wait calls as doing so results in a
  // '500 Interval Server Error' for the default executor leading to it
  // failing fast.
  AWAIT_READY(updateCall1);
  AWAIT_READY(updateCall2);
  AWAIT_READY(waitCall1);
  AWAIT_READY(waitCall2);

  slave.get()->terminate();

  // The TASK_RUNNING updates for the tasks in a task group can be
  // received in any order.
  hashset<v1::TaskID> tasks;

  tasks.insert(taskInfo1.task_id());
  tasks.insert(taskInfo2.task_id());

  Future<v1::executor::Call> subscribeCall =
    FUTURE_HTTP_CALL(Call(), Call::SUBSCRIBE, _, ContentType::PROTOBUF);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Ensure that the executor subscribes again.
  AWAIT_READY(subscribeCall);

  EXPECT_EQ(2, subscribeCall->subscribe().unacknowledged_updates().size());
  EXPECT_EQ(2, subscribeCall->subscribe().unacknowledged_tasks().size());

  // Scheduler should receive the recovered update.
  AWAIT_READY(update1);
  AWAIT_READY(update2);

  EXPECT_EQ(v1::TASK_RUNNING, update1->status().state());
  ASSERT_TRUE(tasks.contains(update1->status().task_id()));

  tasks.erase(update1->status().task_id());

  EXPECT_EQ(v1::TASK_RUNNING, update2->status().state());
  ASSERT_TRUE(tasks.contains(update2->status().task_id()));
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up with recovery=reconnect, make
// sure the executor reregisters and the slave properly sends the update.
TYPED_TEST(SlaveRecoveryTest, ReconnectExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Drop the status updates from the executor.
  // We actually wait until we can drop the TASK_RUNNING update here
  // because the window between the two is small enough that we could
  // still successfully receive TASK_RUNNING after we drop TASK_STARTING.
  Future<StatusUpdateMessage> runningUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  Future<StatusUpdateMessage> startingUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status update is received.
  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);

  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregister =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor reregisters.
  AWAIT_READY(reregister);

  // Executor should inform about the unacknowledged updates.
  ASSERT_LE(1, reregister->updates_size());
  const StatusUpdate& update = reregister->updates(0);
  EXPECT_EQ(task.task_id(), update.status().task_id());
  EXPECT_EQ(TASK_STARTING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_STARTING, status->state());

  driver.stop();
  driver.join();
}


// This ensures that when the executor reconnect retry is enabled,
// the agent will retry the reconnect messages until the executor
// responds. We then ensure that any duplicate re-registration
// messages coming from the executor are ignored.
TYPED_TEST(SlaveRecoveryTest, ReconnectExecutorRetry)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate))
    .WillRepeatedly(Return()); // Ignore subsequent TASK_RUNNING updates.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Pause the clock to ensure the agent does not retry the
  // status update. We will ensure the acknowledgement is
  // checkpointed before we terminate the agent.
  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusUpdate);
  EXPECT_EQ(TASK_STARTING, statusUpdate->state());

  // Ensure the acknowledgement is checkpointed.
  Clock::settle();

  slave.get()->terminate();

  // We drop the first re-registration message to emulate
  // a half-open connection closing for "old" executors
  // that do not have the fix for MESOS-7057.
  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    DROP_PROTOBUF(ReregisterExecutorMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  flags.executor_reregistration_timeout = Seconds(5);
  flags.executor_reregistration_retry_interval = Seconds(1);

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // The first attempt by the executor to reregister is dropped
  // so that the agent will retry the reconnect.
  AWAIT_READY(reregisterExecutorMessage);

  // Now trigger the retry and let the second response through.
  reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Clock::advance(flags.executor_reregistration_retry_interval.get());

  AWAIT_READY(reregisterExecutorMessage);

  // Now ensure that further retries do not occur, since the
  // executor is already reregistered.
  EXPECT_NO_FUTURE_PROTOBUFS(ReregisterExecutorMessage(), _, _);

  Clock::advance(flags.executor_reregistration_retry_interval.get());
  Clock::settle();

  // We have to resume the clock to ensure that the containerizer
  // can reap the executor pid (i.e. the reaper requires a resumed
  // clock).
  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the agent responds to pings from the master while the
// agent is performing recovery. We do this by setting the executor
// re-registration timeout to a duration longer than (agent_ping_timeout *
// (max_agent_ping_timeouts + 1)), and then confirming that the agent is not
// marked unreachable after the max ping timeout has elapsed, even if all
// executors have reregistered. Agent recovery currently does not complete
// until the executor re-registration timeout has elapsed (see MESOS-7539).
TYPED_TEST(SlaveRecoveryTest, PingTimeoutDuringRecovery)
{
  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.agent_ping_timeout = Seconds(1);
  masterFlags.max_agent_ping_timeouts = 2;

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set the executor reregister timeout to a value greater than
  // (agent_ping_timeout * (max_agent_ping_timeouts + 1)).
  slave::Flags slaveFlags = this->CreateSlaveFlags();
  slaveFlags.executor_reregistration_timeout = process::TEST_AWAIT_TIMEOUT;

  Fetcher fetcher(slaveFlags);

  Try<TypeParam*> _containerizer =
    TypeParam::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  const string slavePid = "slave";

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), slavePid, slaveFlags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, false, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  Future<TaskStatus> statusUpdate0;
  Future<TaskStatus> statusUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate0))
    .WillOnce(FutureArg<1>(&statusUpdate1));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusUpdate0);
  ASSERT_EQ(TASK_STARTING, statusUpdate0->state());

  driver.acknowledgeStatusUpdate(statusUpdate0.get());

  AWAIT_READY(statusUpdate1);
  ASSERT_EQ(TASK_RUNNING, statusUpdate1->state());

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acknowledgeStatusUpdate(statusUpdate1.get());

  AWAIT_READY(_statusUpdateAcknowledgement);

  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutor =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<ReregisterSlaveMessage> reregisterSlave =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(
      detector.get(),
      containerizer.get(),
      slavePid,
      slaveFlags);
  ASSERT_SOME(slave);

  // Ensure the executor reregisters.
  AWAIT_READY(reregisterExecutor);

  Clock::pause();

  // Advance the clock enough to ensure that the agent would have been marked
  // unreachable if it was not responding to pings.
  for (unsigned int i = 0; i < masterFlags.max_agent_ping_timeouts + 2; i++) {
    Future<PingSlaveMessage> ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);
    Future<PongSlaveMessage> pong = FUTURE_PROTOBUF(PongSlaveMessage(), _, _);

    Clock::advance(masterFlags.agent_ping_timeout);

    AWAIT_READY(ping);
    AWAIT_READY(pong);
  }

  Clock::resume();

  // Perform reconciliation to verify that the task has not been transitioned to
  // TASK_LOST or TASK_UNREACHABLE, as would occur if the agent had been deemed
  // unreachable.
  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.set_state(TASK_STAGING); // Dummy value.

  statuses.push_back(status);

  Future<TaskStatus> statusUpdate2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusUpdate2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.reconcileTasks(statuses);

  AWAIT_READY(statusUpdate2);
  ASSERT_EQ(TASK_RUNNING, statusUpdate2->state());
  ASSERT_EQ(TaskStatus::SOURCE_MASTER, statusUpdate2->source());
  ASSERT_EQ(TaskStatus::REASON_RECONCILIATION, statusUpdate2->reason());

  // Ensure that the agent has not reregistered yet.
  ASSERT_TRUE(reregisterSlave.isPending());

  // Advance the clock to prompt the agent to reregister.
  Clock::pause();
  Clock::advance(slaveFlags.executor_reregistration_timeout);
  Clock::resume();

  AWAIT_READY(reregisterSlave);

  driver.stop();
  driver.join();
}


// The slave is stopped before the HTTP based command executor is
// registered. When it comes back up with recovery=reconnect, make
// sure the executor is killed and the task is transitioned to LOST.
//
// TODO(alexr): Enable after MESOS-6623 is resolved.
TYPED_TEST(SlaveRecoveryTest, DISABLED_RecoverUnregisteredHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Drop the executor subscribe message.
  Future<v1::executor::Call> subscribeCall =
    DROP_HTTP_CALL(Call(), Call::SUBSCRIBE, _, ContentType::PROTOBUF);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Stop the slave before the executor is subscribed.
  AWAIT_READY(subscribeCall);

  slave.get()->terminate();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::settle();
  Clock::resume();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT,
            status->reason());

  Clock::pause();

  // Master should subsequently reoffer the same resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  Clock::resume();

  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();
}


// The slave is stopped before the (command) executor is registered.
// When it comes back up with recovery=reconnect, make sure the
// executor is killed and the task is transitioned to LOST.
TYPED_TEST(SlaveRecoveryTest, RecoverUnregisteredExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Drop the executor registration message.
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Stop the slave before the executor is registered.
  AWAIT_READY(registerExecutor);

  slave.get()->terminate();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::settle();
  Clock::resume();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT,
            status->reason());

  Clock::pause();

  // Master should subsequently reoffer the same resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  Clock::resume();

  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();
}


// This test verifies that when the agent gets a `killTask`
// message for a queued task on a registering executor, a
// restart of the agent will generate a TASK_KILLED and
// will shut down the executor.
TYPED_TEST(SlaveRecoveryTest, KillQueuedTaskDuringExecutorRegistration)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Drop the executor registration message so that the task stays
  // queued on the agent
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task});

  AWAIT_READY(registerExecutor);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Kill the task enqueued on the agent.
  driver.killTask(task.task_id());

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH, status->reason());

  slave.get()->terminate();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // Restart the agent (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);

  Clock::pause();

  // Ensure the agent considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(executorTerminated);

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// The HTTP command executor terminates when the slave is down.
// When it comes back up with recovery=reconnect, make
// sure the task is properly transitioned to FAILED.
TYPED_TEST(SlaveRecoveryTest, RecoverTerminatedHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return()); // Allow any number of subsequent status updates.

  Future<Nothing> ackRunning =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);
  Future<Nothing> ackStarting =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ackStarting);
  AWAIT_READY(ackRunning);

  slave.get()->terminate();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent status updates.

  // Now shut down the executor, when the slave is down.
  // TODO(qianzhang): Once MESOS-5220 is resolved, we should send a SHUTDOWN
  // event to the executor rather than manually kill it.
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

  ASSERT_SOME(runState.forkedPid);

  ASSERT_SOME(os::killtree(runState.forkedPid.get(), SIGKILL));

  // Ensure the executor is killed before restarting the slave.
  while (os::exists(runState.forkedPid.get())) {
    os::sleep(Milliseconds(100));
  }

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::settle();
  Clock::resume();

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_FAILED, status->state());

  Clock::pause();

  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  Clock::resume();

  // Master should subsequently reoffer the same resources.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// The command executor terminates when the slave is down.
// When it comes back up with recovery=reconnect, make
// sure the task is properly transitioned to LOST.
TYPED_TEST(SlaveRecoveryTest, RecoverTerminatedExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);


  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return()); // Ignore subsequent status updates.

  Future<Nothing> ackRunning =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);
  Future<Nothing> ackStarting =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the TASK_STARTING update from the executor
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  // Wait for the TASK_RUNNING update from the executor
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor->from;

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ackStarting);
  AWAIT_READY(ackRunning);

  slave.get()->terminate();

  Future<TaskStatus> statusLost;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusLost))
    .WillRepeatedly(Return()); // Ignore subsequent status updates.

  // Now shut down the executor, when the slave is down.
  process::post(executorPid, ShutdownExecutorMessage());

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::settle();
  Clock::resume();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(statusLost);

  EXPECT_EQ(TASK_LOST, statusLost->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, statusLost->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT,
            statusLost->reason());

  Clock::pause();

  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  Clock::resume();

  // Master should subsequently reoffer the same resources.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();
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
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  // Set a short recovery timeout, as we can't control the executor
  // driver time when using the process / cgroups isolators.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.recovery_timeout = Milliseconds(1);

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement);

  slave.get()->terminate();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure the executor terminates by causing the recovery timeout
  // to elapse while disconnected from the slave.
  os::sleep(Milliseconds(1));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_FAILED, status->state());

  driver.stop();
  driver.join();
}


// The slave is stopped after an executor is completed (i.e., it
// has terminated and all its updates have been acknowledged).
// When it comes back up with recovery=reconnect, make sure the
// recovery successfully completes and the executor's work and
// meta directories successfully gc'ed.
TYPED_TEST(SlaveRecoveryTest, RecoverCompletedExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], "exit 0");

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(3); // TASK_STARTING, TASK_RUNNING and TASK_FINISHED updates.

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  Future<RegisterExecutorMessage> registerExecutor =
    FUTURE_PROTOBUF(RegisterExecutorMessage(), _, _);

  // We use 'gc.schedule' as a proxy for the cleanup of the executor.
  // The first event will correspond with the finished task, whereas
  // the second is associated with the exited executor.
  vector<Future<Nothing>> schedules = {
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule),
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule)
  };

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor id.
  AWAIT_READY(registerExecutor);
  ExecutorID executorId = registerExecutor->executor_id();

  AWAIT_READY(collect(schedules));

  slave.get()->terminate();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);

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

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


// The slave is stopped before a terminal update is received from the HTTP
// based command executor. The slave is then restarted in recovery=cleanup mode.
// It kills the executor, and terminates. Master should then send TASK_LOST.
//
// TODO(alexr): Enable after MESOS-6623 is resolved.
TYPED_TEST(SlaveRecoveryTest, DISABLED_CleanupHTTPExecutor)
{
  master::Flags masterFlags = this->CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  // Launch a task with the HTTP based command executor.
  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  Future<v1::executor::Call> updateCall =
    DROP_HTTP_CALL(Call(), Call::UPDATE, _, ContentType::PROTOBUF);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Stop the slave before the status updates are received.
  AWAIT_READY(updateCall);

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  ASSERT_EQ(1u, containerIds->size());
  auto containerId = *(containerIds->begin());

  slave.get()->terminate();

  // Slave in cleanup mode shouldn't reregister with the master and
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
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  flags.recover = "cleanup";
  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Wait for recovery to complete.
  AWAIT_READY(__recover);

  // Wait for destruction of the container before advancing the clock, otherwise
  // a containerizer timeout might be triggered causing failure of container
  // destruction, which leads to orphan containers.
  AWAIT_READY(containerizer->wait(containerId));

  Clock::pause();

  // Now advance time until the master marks agent unreachable.
  while (status.isPending()) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  Clock::resume();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, status->reason());

  driver.stop();
  driver.join();
}


// The slave is stopped after a non-terminal update is received.
// Slave is restarted in recovery=cleanup mode. It kills the command
// executor, and terminates. Master should then send TASK_LOST.
TYPED_TEST(SlaveRecoveryTest, CleanupExecutor)
{
  master::Flags masterFlags = this->CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Expect TASK_STARTING and TASK_RUNNING updates
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2)
    .WillRepeatedly(Return()); // Ignore subsequent updates

  Future<Nothing> ackRunning =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ackStarting =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ackStarting);
  AWAIT_READY(ackRunning);

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  ASSERT_EQ(1u, containerIds->size());
  auto containerId = *(containerIds->begin());

  slave.get()->terminate();

  // Slave in cleanup mode shouldn't reregister with the master and
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
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  flags.recover = "cleanup";
  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait for recovery to complete.
  AWAIT_READY(__recover);

  // Wait for destruction of the container before advancing the clock, otherwise
  // a containerizer timeout might be triggered causing failure of container
  // destruction, which leads to orphan containers.
  AWAIT_READY(containerizer->wait(containerId));

  Clock::pause();

  // Now advance time until the master marks agent unreachable.
  while (status.isPending()) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  Clock::resume();

  // Scheduler should receive the TASK_LOST update.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, status->reason());

  driver.stop();
  driver.join();
}


// This test checks whether a non-checkpointing framework is
// properly removed, when a slave is disconnected.
TYPED_TEST(SlaveRecoveryTest, RemoveNonCheckpointingFramework)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(false);

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

  // Launch 2 tasks from this offer.
  vector<TaskInfo> tasks;
  Offer offer = offers.get()[0];

  Offer offer1 = offer;
  Resources resources1 = allocatedResources(
      Resources::parse("cpus:1;mem:512").get(), frameworkInfo.roles(0));
  offer1.mutable_resources()->CopyFrom(resources1);
  // Long-running task.
  tasks.push_back(createTask(offer1, SLEEP_COMMAND(1000)));

  Offer offer2 = offer;
  Resources resources2 = allocatedResources(
      Resources::parse("cpus:1;mem:512").get(), frameworkInfo.roles(0));
  offer2.mutable_resources()->CopyFrom(resources2);
  // Long-running task.
  tasks.push_back(createTask(offer2, SLEEP_COMMAND(1000)));

  ASSERT_TRUE(Resources(offer.resources()).contains(
        Resources(offer1.resources()) +
        Resources(offer2.resources())));

  Future<Nothing> update0;
  Future<Nothing> update1;
  Future<Nothing> update2;
  Future<Nothing> update3;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update0))
    .WillOnce(FutureSatisfy(&update1))
    .WillOnce(FutureSatisfy(&update2))
    .WillOnce(FutureSatisfy(&update3));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait for TASK_STARTING and TASK_RUNNING updates from the tasks.
  AWAIT_READY(update0);
  AWAIT_READY(update1);
  AWAIT_READY(update2);
  AWAIT_READY(update3);

  // The master should generate TASK_LOST updates once the slave is stopped.
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  slave.get()->terminate();

  // Scheduler should receive the TASK_LOST updates.
  AWAIT_READY(status1);
  EXPECT_EQ(TASK_LOST, status1->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status1->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_DISCONNECTED, status1->reason());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_LOST, status2->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status2->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_DISCONNECTED, status2->reason());

  driver.stop();
  driver.join();

  // Destroy all the containers before we destroy the containerizer. We need to
  // do this manually because there are no slaves left in the cluster.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);

  foreach (const ContainerID& containerId, containers.get()) {
    Future<Option<ContainerTermination>> termination =
      containerizer->destroy(containerId);

    AWAIT_READY(termination);
    EXPECT_SOME(termination.get());
  }
}


// This test ensures that no checkpointing happens for a
// framework that has disabled checkpointing.
TYPED_TEST(SlaveRecoveryTest, NonCheckpointingFramework)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Disable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(false);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  Future<Nothing> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&update))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for TASK_RUNNING update.
  AWAIT_READY(update);

  Clock::pause();

  Future<Nothing> updateFramework = FUTURE_DISPATCH(_, &Slave::updateFramework);

  // Set the `FrameworkID` in `FrameworkInfo`.
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  UpdateFrameworkMessage updateFrameworkMessage;
  updateFrameworkMessage.mutable_framework_id()->CopyFrom(frameworkId);
  updateFrameworkMessage.set_pid("");
  updateFrameworkMessage.mutable_framework_info()->CopyFrom(frameworkInfo);

  // Simulate a 'UpdateFrameworkMessage' to ensure framework pid is
  // not being checkpointed.
  process::dispatch(
      slave.get()->pid,
      &Slave::updateFramework,
      updateFrameworkMessage);

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
}


// Scheduler asks a restarted slave to kill a task with HTTP based
// command executor that has been running before the slave restarted.
// This test ensures that a restarted slave is able to communicate
// with all components (scheduler, master, executor).
//
// TODO(alexr): Enable after MESOS-6623 is resolved.
TYPED_TEST(SlaveRecoveryTest, DISABLED_KillTaskWithHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  slave.get()->terminate();

  Future<v1::executor::Call> subscribeCall =
    FUTURE_HTTP_CALL(Call(), Call::SUBSCRIBE, _, ContentType::PROTOBUF);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), id, flags);
  ASSERT_SOME(slave);

  // Wait for the executor to subscribe again.
  AWAIT_READY(subscribeCall);

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. This test ensures that a
// restarted slave is able to communicate with all components
// (scheduler, master, executor).
TYPED_TEST(SlaveRecoveryTest, KillTask)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Expect a TASK_STARTING and a TASK_RUNNING update
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> ack1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ack2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack1);
  AWAIT_READY(ack2);

  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to reregister.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


// When the slave is down we modify the BOOT_ID_FILE to simulate a
// reboot. The subsequent run of the slave should recover.
TYPED_TEST(SlaveRecoveryTest, Reboot)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId1 = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> runningStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&runningStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor ID and PID.
  AWAIT_READY(registerExecutorMessage);

  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage->from;

  // Wait for TASK_RUNNING update.
  AWAIT_READY(runningStatus);

  // Capture the container ID.
  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId1 = *containers->begin();

  slave.get()->terminate();

  // Get the executor's pid so we can reap it to properly simulate a
  // reboot.
  string pidPath = paths::getForkedPidPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId1,
      frameworkId,
      executorId,
      containerId1);

  Try<string> read = os::read(pidPath);
  ASSERT_SOME(read);

  Try<pid_t> pid = numify<pid_t>(read.get());
  ASSERT_SOME(pid);

  Future<Option<int>> executorStatus = process::reap(pid.get());

  // Shut down the executor manually and wait until it's been reaped.
  process::post(slave.get()->pid, executorPid, ShutdownExecutorMessage());

  AWAIT_READY(executorStatus);

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      paths::getBootIdPath(paths::getMetaRootDir(flags.work_dir)),
      "rebooted!"));

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Recover the state.
  Result<slave::state::State> recoverState =
    slave::state::recover(paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(recoverState);
  ASSERT_SOME(recoverState->slave);

  slave::state::SlaveState state = recoverState->slave.get();

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);
  EXPECT_EQ(slaveId1, state.id);

  AWAIT_READY(reregisterSlaveMessage);

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  SlaveID slaveId2 = offers2.get()[0].slave_id();
  EXPECT_EQ(slaveId1, slaveId2);

  ASSERT_TRUE(state.frameworks.contains(frameworkId));

  EXPECT_TRUE(state.frameworks[frameworkId].executors.contains(executorId));

  slave::state::ExecutorState executorState =
    state.frameworks[frameworkId].executors[executorId];

  const Option<ContainerID>& containerId2 = executorState.latest;
  EXPECT_SOME_EQ(containerId1, containerId2);

  EXPECT_TRUE(executorState.runs.contains(containerId2.get()));

  EXPECT_NONE(executorState.runs[containerId2.get()].forkedPid);
  EXPECT_NONE(executorState.runs[containerId2.get()].libprocessPid);

  EXPECT_TRUE(executorState
                .runs[containerId2.get()]
                .tasks.contains(task.task_id()));

  driver.stop();
  driver.join();
}


// When the slave is down we modify the BOOT_ID_FILE and the executor's forked
// pid file to simulate executor's pid is reused after agent host is rebooted,
// the framework should receive `TASK_FAILED` status update after the reboot.
// This is a regression test for MESOS-9501.
TYPED_TEST(SlaveRecoveryTest, RebootWithExecutorPidReused)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId1 = offers.get()[0].slave_id();
  FrameworkID frameworkId = offers.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&failedStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> startingAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> runningAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the executor ID and PID.
  AWAIT_READY(registerExecutorMessage);

  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage->from;

  // Wait for TASK_STARTING and TASK_RUNNING updates and their ACKs
  // to make sure the next sent status update is not a repeat of the
  // unacknowledged TASK_RUNNING.
  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(startingAck);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  AWAIT_READY(runningAck);

  // Capture the container ID.
  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *containers->begin();

  slave.get()->terminate();

  // Get the executor's pid so we can reap it to properly simulate a
  // reboot.
  string pidPath = paths::getForkedPidPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId1,
      frameworkId,
      executorId,
      containerId);

  Try<string> read = os::read(pidPath);
  ASSERT_SOME(read);

  Try<pid_t> pid = numify<pid_t>(read.get());
  ASSERT_SOME(pid);

  Future<Option<int>> executorStatus = process::reap(pid.get());

  // Shut down the executor manually and wait until it's been reaped.
  process::post(slave.get()->pid, executorPid, ShutdownExecutorMessage());

  AWAIT_READY(executorStatus);

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      paths::getBootIdPath(paths::getMetaRootDir(flags.work_dir)),
      "rebooted!"));

  // Modify the executor's pid to simulate the pid is reused after reboot.
  ASSERT_SOME(os::write(pidPath, stringify(::getpid())));

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlaveMessage);

  AWAIT_READY(failedStatus);
  EXPECT_EQ(TASK_FAILED, failedStatus->state());

  driver.stop();
  driver.join();
}


// When the agent is down we modify the BOOT_ID_FILE to simulate a
// reboot and change the resources flag to cause a mismatch
// between the recovered agent's agent info and the one of the new agent
// to ensure it registers as a new agent.
TYPED_TEST(SlaveRecoveryTest, RebootWithSlaveInfoMismatch)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = "cpus:8;mem:4096;disk:2048";

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  // Capture offer in order to get the agent ID.
  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);

  SlaveID slaveId1 = offers1.get()[0].slave_id();

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  slave.get()->terminate();

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      paths::getBootIdPath(paths::getMetaRootDir(flags.work_dir)),
      "rebooted!"));

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // Change agent's resources to cause agent Info mismatch.
  flags.resources = "cpus:4;mem:2048;disk:2048";

  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  // Recover the state.
  Result<slave::state::State> recoverState =
    slave::state::recover(paths::getMetaRootDir(flags.work_dir), true);

  // Capture offer in order to get the new agent ID.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ASSERT_SOME(recoverState);
  EXPECT_SOME(recoverState->slave);

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  AWAIT_READY(offers2);

  ASSERT_EQ(1u, offers2->size());
  Offer offer2 = offers2.get()[0];

  SlaveID slaveId2 = offers2.get()[0].slave_id();

  EXPECT_NE(slaveId1, slaveId2);

  driver.stop();
  driver.join();
}


// When the agent is down we modify the BOOT_ID_FILE to simulate a
// reboot and change the resources flag to cause a mismatch between
// the recovered agent's info and the one of the new agent to make it
// recover as a new one. We restart the recovered agent before it
// registers with the master to ensure it still recovers as a new
// agent after that.
TYPED_TEST(SlaveRecoveryTest, RebootWithSlaveInfoMismatchAndRestart)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = "cpus:8;mem:4096;disk:2048";

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  // Capture offer in order to get the agent ID.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());
  SlaveID slaveId1 = offers.get()[0].slave_id();

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  slave.get()->terminate();

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      paths::getBootIdPath(paths::getMetaRootDir(flags.work_dir)),
      "rebooted!"));

  // Change agent's resources to cause agent info mismatch.
  flags.resources = "cpus:4;mem:2048;disk:2048";

  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  // Capture offer in order to get the new agent ID.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Clock::pause();
  Clock::settle();

  // Simulate an agent restart before reregistration completes.
  Future<RegisterSlaveMessage> registerSlaveMessage =
    DROP_PROTOBUF(RegisterSlaveMessage(), _, master.get()->pid);

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::advance(flags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage);

  slave.get()->terminate();

  Clock::resume();

  // Verify that the last agent instance removed the "latest" symlink
  // when it detected agent info discrepancy.
  const string latest =
    paths::getLatestSlavePath(paths::getMetaRootDir(flags.work_dir));
  ASSERT_FALSE(os::exists(latest));

  // Start the agent again to verify that it still successfully
  // recovers as a new one.
  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Verify that the agent registered as a new one.
  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());
  EXPECT_NE(slaveId1, offers->at(0).slave_id());

  driver.stop();
  driver.join();
}


// When the slave is down we remove the "latest" symlink in the
// executor's run directory, to simulate a situation where the
// recovered slave (--no-strict) cannot recover the executor and
// hence schedules it for gc.
TYPED_TEST(SlaveRecoveryTest, GCExecutor)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.strict = false;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  Future<RegisterExecutorMessage> registerExecutor =
    FUTURE_PROTOBUF(RegisterExecutorMessage(), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor id.
  AWAIT_READY(registerExecutor);
  ExecutorID executorId = registerExecutor->executor_id();

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  // NOTE: We want to destroy all running containers.
  slave->reset();

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
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);

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
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


// When the slave is down we remove the latest run directory
// but not the "latest" symlink, to simulate a situation where the
// slave died in the middle of gc'ing the run meta directory.
TYPED_TEST(SlaveRecoveryTest, ExecutorDanglingLatestSymlink)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.strict = true;

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1)).WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  Future<RegisterExecutorMessage> registerExecutor =
    FUTURE_PROTOBUF(RegisterExecutorMessage(), _, _);

  Future<Nothing> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor id.
  AWAIT_READY(registerExecutor);
  ExecutorID executorId = registerExecutor->executor_id();

  // Wait for TASK_RUNNING update.
  AWAIT_READY(status);

  // Terminate the slave.
  slave.get()->terminate();

  // The "latest" symlink should exist.
  const string latestPath = paths::getExecutorLatestRunPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId,
      frameworkId,
      executorId);
  ASSERT_TRUE(os::exists(latestPath));
  // And should point to the latest run.
  const Result<string> path = os::realpath(latestPath);
  ASSERT_SOME(path);
  // Delete it - "latest" will now dangle.
  ASSERT_SOME(os::rmdir(path.get(), true));

  // Recover the state.
  Result<slave::state::State> recoverState =
    slave::state::recover(paths::getMetaRootDir(flags.work_dir), true);

  ASSERT_SOME(recoverState);
  ASSERT_SOME(recoverState->slave);

  // The executor should be recovered without any run.
  slave::state::FrameworkState frameworkState =
    recoverState->slave->frameworks.at(frameworkId);
  ASSERT_EQ(1u, frameworkState.executors.size());
  slave::state::ExecutorState& executorState =
    frameworkState.executors.at(executorId);
  ASSERT_NONE(executorState.latest);
  ASSERT_TRUE(executorState.runs.empty());

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // The slave should re-register.
  AWAIT_READY(reregisterSlaveMessage);

  // Wait until executor's work and meta directories get gc'ed.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();

  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      flags.work_dir, slaveId, frameworkId, executorId)));
  ASSERT_FALSE(os::exists(paths::getExecutorPath(
      paths::getMetaRootDir(flags.work_dir),
      slaveId,
      frameworkId,
      executorId)));

  driver.stop();
  driver.join();
}


// The slave is asked to shutdown. When it comes back up, it should
// reregister as the same agent.
TYPED_TEST(SlaveRecoveryTest, ShutdownSlave)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))  // Initial offer.
    .WillOnce(FutureArg<1>(&offers2)); // Task resources re-offered.

  driver.start();

  AWAIT_READY(offers1);

  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  Future<Nothing> statusUpdate1, statusUpdate2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&statusUpdate1)) // TASK_STARTING
    .WillOnce(FutureSatisfy(&statusUpdate2)) // TASK_RUNNING
    .WillOnce(Return());  // Ignore TASK_FAILED update.

  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Capture the executor pid.
  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor->from;

  AWAIT_READY(statusUpdate2); // Wait for TASK_RUNNING update.

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtMost(1));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // We shut down the executor here so that a shutting down slave
  // does not spend too much time waiting for the executor to exit.
  process::post(slave.get()->pid, executorPid, ShutdownExecutorMessage());

  AWAIT_READY(executorTerminated);
  AWAIT_READY(offers2);

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  // Explicitly shutdown the slave.
  slave.get()->shutdown();
  slave->reset();

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Now restart the slave (use same flags) with a new containerizer.
  Try<TypeParam*> containerizer2 = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure that the slave registered with a new id.
  AWAIT_READY(offers3);

  ASSERT_FALSE(offers3->empty());
  // Make sure all slave resources are reoffered.
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers3.get()[0].resources()));

  // Ensure the slave id is same.
  EXPECT_EQ(
      offers1.get()[0].slave_id().value(), offers3.get()[0].slave_id().value());

  driver.stop();
  driver.join();
}


#ifndef __WINDOWS__
// The slave should shutdown when it receives a SIGUSR1 signal.
//
// TODO(andschwa): The Windows agent shuts down with SIGUSR1, but it does so
// through a custom signal-handler for Ctrl-C, not through `kill`, so this test
// can be implemented later. See MESOS-8505.
TYPED_TEST(SlaveRecoveryTest, ShutdownSlaveSIGUSR1)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))  // Initial offer.
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting, statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<TaskStatus> statusLost;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusLost));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(_, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<Nothing> signaled =
    FUTURE_DISPATCH(_, &Slave::signaled);

  Future<UnregisterSlaveMessage> unregisterSlaveMessage =
    FUTURE_PROTOBUF(
        UnregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  // Send SIGUSR1 signal to the slave.
  kill(getpid(), SIGUSR1);

  AWAIT_READY(signaled);
  AWAIT_READY(unregisterSlaveMessage);
  AWAIT_READY(executorTerminated);

  // The master should send a TASK_LOST and slaveLost.
  AWAIT_READY(statusLost);

  EXPECT_EQ(TASK_LOST, statusLost->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, statusLost->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, statusLost->reason());

  AWAIT_READY(slaveLost);

  // Make sure the slave terminates.
  ASSERT_TRUE(process::wait(slave.get()->pid, Seconds(10)));

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// The slave fails to do recovery and tries to register as a new slave. The
// master should give it a new id and transition all the tasks of the old slave
// to LOST.
TYPED_TEST(SlaveRecoveryTest, RegisterDisconnectedSlave)
{
  master::Flags masterFlags = this->CreateMasterFlags();

  // Disable authentication so the spoofed re-registration below works.
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId = offers.get()[0].slave_id();
  FrameworkID frameworkId = offers.get()[0].framework_id();

  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<Nothing> startingStatus;
  Future<Nothing> runningStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureSatisfy(&startingStatus))
    .WillOnce(FutureSatisfy(&runningStatus));

  Future<Nothing> startingAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);
  Future<Nothing> runningAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutorMessage);

  // Wait for the acknowledgement of the TASK_RUNNING update
  // to make sure the next sent status update is not a repeat
  // of the unacknowledged TASK_RUNNING.
  AWAIT_READY(startingStatus);
  AWAIT_READY(startingAck);
  AWAIT_READY(runningStatus);
  AWAIT_READY(runningAck);

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(AtMost(1));

  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  slave.get()->terminate();

  // Spoof the registration attempt of a slave that failed recovery.
  // We do this because simply restarting the slave will result in a slave
  // with a different pid than the previous one.
  post(slave.get()->pid, master.get()->pid, registerSlaveMessage.get());

  // Scheduler should get a TASK_LOST update.
  AWAIT_READY(lostStatus);

  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, lostStatus->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());

  // TODO(neilc): We need to destroy the slave here to avoid the
  // metrics request hanging (MESOS-6231).
  slave->reset();

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
  EXPECT_EQ(0, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(0, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_registered"]);

  driver.stop();
  driver.join();

  // Destroy all the containers before we destroy the containerizer. We need to
  // do this manually because there are no slaves left in the cluster.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);

  foreach (const ContainerID& containerId, containers.get()) {
    Future<Option<ContainerTermination>> termination =
      containerizer->destroy(containerId);

    AWAIT_READY(termination);
    EXPECT_SOME(termination.get());
  }
}


// This test verifies that a KillTask message received by the master when a
// slave is disconnected is properly reconciled when the slave reregisters.
TYPED_TEST(SlaveRecoveryTest, ReconcileKillTask)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
      FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Capture the slave and framework ids.
  SlaveID slaveId = offers1.get()[0].slave_id();
  FrameworkID frameworkId = offers1.get()[0].framework_id();

  // Expecting TASK_STARTING and TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for TASK_RUNNING update to be acknowledged.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  slave.get()->terminate();

  // Now send a KillTask message to the master. This will not be
  // received by the slave because it is down.
  driver.killTask(task.task_id());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Now restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Scheduler should get a TASK_KILLED message.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  driver.stop();
  driver.join();
}


// This test verifies that when the slave recovers and reregisters
// with a framework that was shutdown when the slave was down, it gets
// a ShutdownFramework message.
TYPED_TEST(SlaveRecoveryTest, ReconcileShutdownFramework)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  // Capture the framework id.
  FrameworkID frameworkId = offers.get()[0].framework_id();

  // Expecting a TASK_STARTING and a TASK_RUNNING status.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2)
    .WillRepeatedly(Return());

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the updates to be acknowledged.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  slave.get()->terminate();

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
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Slave should get a ShutdownFrameworkMessage.
  AWAIT_READY(shutdownFrameworkMessage);

  // Ensure that the executor is terminated.
  AWAIT_READY(executorTerminated);

  // Check the output of the master's "/state" endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  EXPECT_TRUE(parse->values["frameworks"].as<JSON::Array>().values.empty());
  EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

  JSON::Array completedFrameworks =
    parse->values["completed_frameworks"].as<JSON::Array>();

  ASSERT_EQ(1u, completedFrameworks.values.size());

  JSON::Object completedFramework =
    completedFrameworks.values.front().as<JSON::Object>();

  EXPECT_EQ(
      frameworkId,
      completedFramework.values["id"].as<JSON::String>().value);

  JSON::Array completedTasks =
    completedFramework.values["completed_tasks"].as<JSON::Array>();

  ASSERT_EQ(1u, completedTasks.values.size());

  JSON::Object completedTask = completedTasks.values.front().as<JSON::Object>();

  EXPECT_EQ(
      "TASK_KILLED",
      completedTask.values["state"].as<JSON::String>().value);
}


// This ensures that reconciliation properly deals with tasks
// present in the master and missing from the slave. Notably:
//   1. The tasks are sent to DROPPED.
//   2. The task resources are recovered.
// TODO(bmahler): Ensure the executor resources are recovered by
// using an explicit executor.
TYPED_TEST(SlaveRecoveryTest, ReconcileTasksMissingFromSlave)
{
  TestAllocator<master::allocator::HierarchicalDRFAllocator> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing and partition-awareness for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework_(_, _, _, _, _));

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Start a task on the slave so that the master has knowledge of it.
  // We'll ensure the slave does not have this task when it
  // reregisters by wiping the relevant meta directory.
  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(10));

  Future<TaskStatus> starting;
  Future<TaskStatus> running;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&starting))
    .WillOnce(FutureArg<1>(&running))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> startingAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> runningAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(starting);
  AWAIT_READY(startingAck);

  AWAIT_READY(running);
  AWAIT_READY(runningAck);

  EXPECT_EQ(TASK_STARTING, starting->state());
  EXPECT_EQ(TASK_RUNNING, running->state());

  EXPECT_CALL(allocator, deactivateSlave(_));

  slave.get()->terminate();

  // Reset the slave so that the task status update stream destructors are
  // called. This is necessary so that file handles are closed before we remove
  // the framework meta directory (specifically for `task.updates`).
  slave->reset();

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
  ASSERT_SOME(state->slave);
  ASSERT_TRUE(state->slave->frameworks.contains(frameworkId.get()));

  slave::state::FrameworkState frameworkState =
    state->slave->frameworks.at(frameworkId.get());

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
  EXPECT_CALL(allocator, recoverResources(_, _, _, _, _));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  // Wait for TASK_DROPPED update.
  AWAIT_READY(status);

  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_DROPPED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, status->reason());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  // If there was an outstanding offer, we can get a call to
  // recoverResources when we stop the scheduler.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _, _))
    .WillRepeatedly(Return());

  driver.stop();
  driver.join();
}


// Scheduler asks a restarted slave to kill a task that has been
// running before the slave restarted. A scheduler failover happens
// when the slave is down. This test verifies that a scheduler
// failover will not affect the slave recovery process.
TYPED_TEST(SlaveRecoveryTest, SchedulerFailover)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler.
  FrameworkInfo framework1 = DEFAULT_FRAMEWORK_INFO;
  framework1.set_checkpoint(true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, framework1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Create a long running task.
  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  // Expecting TASK_STARTING and TASK_RUNNING updates
  EXPECT_CALL(sched1, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offers1.get()[0].id(), {task});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  slave.get()->terminate();

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());
  framework2.set_checkpoint(true);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

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
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to reregister.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched2, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill the task.
  driver2.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  Clock::pause();

  // Advance the clock until the allocator allocates
  // the recovered resources.
  while (offers2.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  Clock::resume();

  driver2.stop();
  driver2.join();

  driver1.stop();
  driver1.join();
}


// This test verifies that if the master changes when the slave is
// down, the slave can still recover the task when it restarts. We
// verify its correctness by killing the task from the scheduler.
//
// TODO(andschwa): Enable when replicated log is supported (MESOS-5932).
TYPED_TEST_TEMP_DISABLED_ON_WINDOWS(SlaveRecoveryTest, MasterFailover)
{
  // Step 1. Run a task.
  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.registry = "replicated_log";
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()->pid));

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, detector.get(), frameworkInfo);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2); // TASK_STARTING and TASK_RUNNING

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task});

  // Wait for both ACKs to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  slave.get()->terminate();

  // Step 2. Simulate failed over master by restarting the master.
  EXPECT_CALL(sched, disconnected(&driver));

  master->reset();
  master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Simulate a new master detected event to the scheduler.
  detector->appoint(master.get()->pid);

  // Framework should get a registered callback.
  AWAIT_READY(registered);

  // Step 3. Restart the slave and kill the task.
  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new isolator.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executor to reregister.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Wait for the executor to terminate before shutting down the
  // slave in order to give cgroups (if applicable) time to clean up.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  // Kill the task.
  driver.killTask(task.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  // Make sure all slave resources are reoffered.
  AWAIT_READY(offers2);
  EXPECT_EQ(Resources(offers1.get()[0].resources()),
            Resources(offers2.get()[0].resources()));

  AWAIT_READY(executorTerminated);

  driver.stop();
  driver.join();
}


// In this test there are two frameworks and one slave. Each
// framework launches a task before the slave goes down. We verify
// that the two frameworks and their tasks are recovered after the
// slave restarts.
TYPED_TEST(SlaveRecoveryTest, MultipleFrameworks)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<TypeParam*> _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Framework 1. Enable checkpointing.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_checkpoint(true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Use part of the resources in the offer so that the rest can be
  // offered to framework 2.
  Offer offer1 = offers1.get()[0];
  offer1.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:512").get());

  // Framework 1 launches a task.
  TaskInfo task1 = createTask(offer1, SLEEP_COMMAND(1000));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _startingStatusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _runningStatusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offer1.id(), {task1});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_startingStatusUpdateAcknowledgement1);
  AWAIT_READY(_runningStatusUpdateAcknowledgement1);

  // Framework 2. Enable checkpointing.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_checkpoint(true);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver2.start();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // Framework 2 launches a task.
  TaskInfo task2 = createTask(offers2.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched2, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _startingStatusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _runningStatusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver2.launchTasks(offers2.get()[0].id(), {task2});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_startingStatusUpdateAcknowledgement2);
  AWAIT_READY(_runningStatusUpdateAcknowledgement2);

  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage2 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);
  Future<ReregisterExecutorMessage> reregisterExecutorMessage1 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = TypeParam::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Wait for the executors to reregister.
  AWAIT_READY(reregisterExecutorMessage1);
  AWAIT_READY(reregisterExecutorMessage2);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  // Expectations for the status changes as a result of killing the
  // tasks.
  Future<TaskStatus> status1;
  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<TaskStatus> status2;
  EXPECT_CALL(sched2, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

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
  EXPECT_EQ(TASK_KILLED, status1->state());

  // Kill task 2.
  driver2.killTask(task2.task_id());

  // Wait for TASK_KILLED update.
  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2->state());

  AWAIT_READY(executorTerminated1);
  AWAIT_READY(executorTerminated2);

  driver1.stop();
  driver1.join();
  driver2.stop();
  driver2.join();
}


// This test verifies that slave recovery works properly even if
// multiple slaves are co-located on the same host.
TYPED_TEST(SlaveRecoveryTest, MultipleSlaves)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  driver.start();

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Start the first slave.
  slave::Flags flags1 = this->CreateSlaveFlags();

#ifndef __WINDOWS__
  // NOTE: We cannot run multiple slaves simultaneously on a host if cgroups
  // isolation is involved. Since this does not apply to Windows, we use the
  // default flags from `CreateSlaveFlags()` above.
  flags1.isolation = "filesystem/posix,posix/mem,posix/cpu";
#endif // __WINDOWS__

  Fetcher fetcher(flags1);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<TypeParam*> _containerizer1 = TypeParam::create(flags1, true, &fetcher);
  ASSERT_SOME(_containerizer1);
  Owned<slave::Containerizer> containerizer1(_containerizer1.get());

  Try<Owned<cluster::Slave>> slave1 =
    this->StartSlave(detector.get(), containerizer1.get(), flags1);
  ASSERT_SOME(slave1);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Launch a long running task in the first slave.
  TaskInfo task1 = createTask(offers1.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _startingStatusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(slave1.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _runningStatusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(slave1.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_startingStatusUpdateAcknowledgement1);
  AWAIT_READY(_runningStatusUpdateAcknowledgement1);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // Start the second slave.
  slave::Flags flags2 = this->CreateSlaveFlags();

#ifndef __WINDOWS__
  // NOTE: We cannot run multiple slaves simultaneously on a host if cgroups
  // isolation is involved. Since this does not apply to Windows, we use the
  // default flags from `CreateSlaveFlags()` above.
  flags2.isolation = "filesystem/posix,posix/mem,posix/cpu";
#endif // __WINDOWS__

  Try<TypeParam*> _containerizer2 = TypeParam::create(flags2, true, &fetcher);
  ASSERT_SOME(_containerizer2);
  Owned<slave::Containerizer> containerizer2(_containerizer2.get());

  Try<Owned<cluster::Slave>> slave2 =
    this->StartSlave(detector.get(), containerizer2.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // Launch a long running task in each slave.
  TaskInfo task2 = createTask(offers2.get()[0], SLEEP_COMMAND(1000));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _startingStatusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave2.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _runningStatusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(slave2.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers2.get()[0].id(), {task2});

  // Wait for the ACKs to be checkpointed.
  AWAIT_READY(_startingStatusUpdateAcknowledgement2);
  AWAIT_READY(_runningStatusUpdateAcknowledgement2);

  slave1.get()->terminate();
  slave2.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage2 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);
  Future<ReregisterExecutorMessage> reregisterExecutorMessage1 =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage2 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);
  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart both slaves using the same flags with new containerizers.
  _containerizer1 = TypeParam::create(flags1, true, &fetcher);
  ASSERT_SOME(_containerizer1);
  containerizer1.reset(_containerizer1.get());

  Clock::pause();

  slave1 = this->StartSlave(detector.get(), containerizer1.get(), flags1);
  ASSERT_SOME(slave1);

  _containerizer2 = TypeParam::create(flags2, true, &fetcher);
  ASSERT_SOME(_containerizer2);
  containerizer2.reset(_containerizer2.get());

  slave2 = this->StartSlave(detector.get(), containerizer2.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(reregisterExecutorMessage1);
  AWAIT_READY(reregisterExecutorMessage2);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags1.executor_reregistration_timeout);
  Clock::resume();

  // Wait for the slaves to reregister.
  AWAIT_READY(slaveReregisteredMessage1);
  AWAIT_READY(slaveReregisteredMessage2);

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> executorTerminated2 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);
  Future<Nothing> executorTerminated1 =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill both tasks.
  driver.killTask(task1.task_id());
  driver.killTask(task2.task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_KILLED, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2->state());

  AWAIT_READY(executorTerminated1);
  AWAIT_READY(executorTerminated2);

  driver.stop();
  driver.join();
}


// The slave is stopped after it dispatched Containerizer::launch but
// before the containerizer has processed the launch. When the slave
// comes back up it should send a TASK_FAILED for the task.
// NOTE: This is a 'TYPED_TEST' but we don't use 'TypeParam'.
TYPED_TEST(SlaveRecoveryTest, RestartBeforeContainerizerLaunch)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  TestContainerizer containerizer1;
  TestContainerizer containerizer2;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer1, flags);

  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Expect the launch but don't do anything.
  Future<Nothing> launch;
  EXPECT_CALL(containerizer1, launch(_, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&launch),
                    Return(Future<Containerizer::LaunchResult>())));

  // No status update should be sent for now.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(0);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Once we see the call to launch, restart the slave.
  AWAIT_READY(launch);

  slave.get()->terminate();

  Future<TaskStatus> status;

  // There is a race here where the Slave may reregister before we
  // shut down. If it does, it causes the TaskStatusUpdateManager to
  // flush which will cause a duplicate status update to be sent. As
  // such, we ignore any subsequent updates.
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Clock::pause();

  slave = this->StartSlave(detector.get(), &containerizer2, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);
  Clock::resume();

  // Scheduler should receive the TASK_FAILED update.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED,
            status->reason());

  driver.stop();
  driver.join();
}


// This test starts a task, restarts the slave with increased
// resources while the task is still running, and then stops
// the task, verifying that all resources are seen in subsequent
// offers.
TYPED_TEST(SlaveRecoveryTest, AgentReconfigurationWithRunningTask)
{
  Clock::pause();

  // Start a master.
  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start a framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Start a slave.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // NOTE: These tests will start with "zero" memory, and the default Windows
  // isolators will enforce this, causing the task to crash. The default POSIX
  // isolators don't actually perform isolation, and so this does not occur.
  // However, these tests are not testing isolation, they're testing resource
  // accounting, so we can just use "no" isolators.
  slaveFlags.isolation = "";
  slaveFlags.resources = "cpus:5;mem:0;disk:0;ports:0";

  Fetcher fetcher(slaveFlags);

  Try<TypeParam*> _containerizer = TypeParam::create(
      slaveFlags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), slaveFlags);

  ASSERT_SOME(slave);

  // Start a long-running task on the slave.
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  EXPECT_EQ(
      offers1.get()[0].resources(),
      allocatedResources(Resources::parse("cpus:5").get(), "*"));

  SlaveID slaveId = offers1.get()[0].slave_id();
  TaskInfo task = createTask(
      slaveId, Resources::parse("cpus:3").get(), SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusKilled));

  // Explicitly set the `refuse_seconds` offer filter to 0,
  // because the default five seconds can be enough to cause
  // timeouts when awaiting the subsequent offer on heavily
  // loaded systems. (MESOS-9358)
  Filters filters;
  filters.set_refuse_seconds(0);
  driver.launchTasks(offers1.get()[0].id(), {task}, filters);

  AWAIT_READY(statusStarting);
  AWAIT_READY(statusRunning);

  // Grab one of the offers while the task is running.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  EXPECT_EQ(
      offers2.get()[0].resources(),
      allocatedResources(Resources::parse("cpus:2").get(), "*"));

  driver.declineOffer(offers2.get()[0].id());


  // Restart the slave with increased resources.
  slave.get()->terminate();
  slaveFlags.reconfiguration_policy = "additive";
  slaveFlags.resources = "cpus:10;mem:512;disk:0;ports:0";

  // Remove the registration delay to circumvent a delay in the recovery
  // logic. This will allow the agent to register with the master after
  // detecting the master, even when the clock is paused.
  slaveFlags.registration_backoff_factor = Seconds(0);

  // Restart the slave with a new containerizer.
  _containerizer = TypeParam::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Grab one of the offers after the slave was restarted.
  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3));

  slave = this->StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Wait for the executor and then skip forward to trigger the executor
  // reregistration timeout, which resumes the agent recovery.
  AWAIT_READY(reregisterExecutorMessage);
  Clock::advance(slaveFlags.executor_reregistration_timeout);

  AWAIT_READY(slaveReregistered);

  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers3);
  EXPECT_EQ(
      offers3.get()[0].resources(),
      allocatedResources(Resources::parse("cpus:7;mem:512").get(), "*"));

  // Decline so we get the resources offered again with the next offer.
  driver.declineOffer(offers3.get()[0].id());

  // Kill the task
  driver.killTask(task.task_id());
  AWAIT_READY(statusKilled);
  ASSERT_EQ(TASK_KILLED, statusKilled->state());

  // Grab one of the offers after the task was killed.
  Future<vector<Offer>> offers4;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers4));

  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers4);
  EXPECT_EQ(
      offers4.get()[0].resources(),
      allocatedResources(Resources::parse("cpus:10;mem:512").get(), "*"));

  driver.stop();
  driver.join();
}


// This test verifies that checkpointed resources sent by a non resource
// provider-capable agent during agent reregistration are ignored by
// the master and that the master reattempts the checkpointing.
TYPED_TEST(SlaveRecoveryTest, CheckpointedResources)
{
  Clock::pause();

  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // The master only sends `CheckpointResourcesMessage` to
  // agents which are not resource provider-capable.
  slaveFlags.agent_features = SlaveCapabilities();

  foreach (
      const SlaveInfo::Capability& slaveCapability,
      slave::AGENT_CAPABILITIES()) {
    if (slaveCapability.type() != SlaveInfo::Capability::RESOURCE_PROVIDER) {
      slaveFlags.agent_features->add_capabilities()->CopyFrom(slaveCapability);
    }
  }

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger the agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Below we send a reserve operation which is applied in the master
  // speculatively. We drop the operation on its way to the agent so
  // that the master's and the agent's view of the agent's
  // checkpointed resources diverge.
  Future<CheckpointResourcesMessage> checkpointResourcesMessage =
    DROP_PROTOBUF(CheckpointResourcesMessage(), _, _);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  const Offer& offer1 = offers1->at(0);

  const Resources offeredResources = offer1.resources();
  const Resources dynamicallyReserved =
    offeredResources.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  driver.acceptOffers({offer1.id()}, {RESERVE(dynamicallyReserved)}, filters);

  AWAIT_READY(checkpointResourcesMessage);

  // Restart and reregister the agent.
  slave.get()->terminate();

  slave = this->StartSlave(&detector, slaveFlags);

  // Advance the clock to trigger an agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Advance the clock to trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  const Offer& offer2 = offers2->at(0);

  // The second offer will be identical to the first
  // one as our operations never made it to the agent.
  const Resources offeredResources2 = offer2.resources();

  EXPECT_EQ(dynamicallyReserved, offeredResources2);
}


// This test verifies that checkpointed resources sent by resource
// provider-capable agents during agent reregistration overwrite the
// master's view of that agent's checkpointed resources.
TYPED_TEST(SlaveRecoveryTest, CheckpointedResourcesResourceProviderCapable)
{
  Clock::pause();

  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger the agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Below we send a reserve operation which is applied in the master
  // speculatively. We drop the operation on its way to the agent so
  // that the master's and the agent's view of the agent's
  // checkpointed resources diverge.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  const Offer& offer1 = offers1->at(0);

  const Resources offeredResources = offer1.resources();
  const Resources dynamicallyReserved =
    offeredResources.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  driver.acceptOffers({offer1.id()}, {RESERVE(dynamicallyReserved)}, filters);

  AWAIT_READY(applyOperationMessage);

  // Restart and reregister the agent.
  slave.get()->terminate();
  slave = this->StartSlave(&detector, slaveFlags);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Advance the clock to trigger an agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  // Advance the clock to trigger a batch allocation.
  AWAIT_READY(updateSlaveMessage);

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  const Offer& offer2 = offers2->at(0);

  // The second offer will be identical to the first
  // one as our operations never made it to the agent.
  const Resources offeredResources1 = offer1.resources();
  const Resources offeredResources2 = offer2.resources();

  EXPECT_EQ(offeredResources1, offeredResources2);
}


// We explicitly instantiate a SlaveRecoveryTest for test cases where
// we assume we'll only have the MesosContainerizer.
class MesosContainerizerSlaveRecoveryTest
  : public SlaveRecoveryTest<MesosContainerizer> {};


// Tests that the containerizer will properly report resource limits
// after an agent failover.
TEST_F(MesosContainerizerSlaveRecoveryTest, ResourceStatistics)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  // Wait until the task is running.
  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  slave.get()->terminate();

  // Wait until the executor re-registered and task resource updated.
  Future<Message> reregisterExecutor =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterExecutor);
  AWAIT_READY(update);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // Check the resource limits are set.
  EXPECT_TRUE(usage->has_cpus_soft_limit());
  EXPECT_TRUE(usage->has_mem_soft_limit_bytes());
  EXPECT_TRUE(usage->has_mem_limit_bytes());

  // Destroy the container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  driver.stop();
  driver.join();
}


#ifdef __linux__
// Test that a container started without namespaces/pid isolation can
// be destroyed correctly with namespaces/pid isolation enabled.
TEST_F(MesosContainerizerSlaveRecoveryTest, ROOT_CGROUPS_PidNamespaceForward)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave using a containerizer without pid namespace
  // isolation.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Scheduler expectations.
  EXPECT_CALL(sched, registered(_, _, _));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  SlaveID slaveId = offers1.get()[0].slave_id();

  TaskInfo task1 = createTask(
      slaveId, Resources::parse("cpus:0.5;mem:128").get(), SLEEP_COMMAND(1000));

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(registerExecutorMessage);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  // Wait until task is running.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Stop the slave.
  slave.get()->terminate();

  // Start a slave using a containerizer with pid namespace isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem,filesystem/linux,namespaces/pid";

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2->empty());

  // Destroy the container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  driver.stop();
  driver.join();
}


// Test that a container started with namespaces/pid isolation can
// be destroyed correctly without namespaces/pid isolation enabled.
TEST_F(MesosContainerizerSlaveRecoveryTest, ROOT_CGROUPS_PidNamespaceBackward)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave using a containerizer with pid namespace isolation.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Scheduler expectations.
  EXPECT_CALL(sched, registered(_, _, _));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  SlaveID slaveId = offers1.get()[0].slave_id();

  TaskInfo task1 = createTask(
      slaveId, Resources::parse("cpus:0.5;mem:128").get(), SLEEP_COMMAND(1000));

  // Message expectations.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(registerExecutorMessage);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  // Wait until task is running.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Stop the slave.
  slave.get()->terminate();

  // Start a slave using a containerizer without pid namespace
  // isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem";

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  slave = this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers2);
  EXPECT_FALSE(offers2->empty());

  // Destroy the container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  driver.stop();
  driver.join();
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
