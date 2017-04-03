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

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/resources.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>

#include <stout/os/exists.hpp>

#include "slave/paths.hpp"

#include "tests/cluster.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Future;
using process::Owned;

using process::http::OK;
using process::http::Response;

using std::pair;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// Tests that exercise the default executor implementation
// should be located in this file.

class DefaultExecutorTest
  : public MesosTest,
    public WithParamInterface<string>
{
protected:
  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifndef USE_SSL_SOCKET
    // Disable operator API authentication for the default executor. Executor
    // authentication currently has SSL as a dependency, so we cannot require
    // executors to authenticate with the agent operator API if Mesos was not
    // built with SSL support.
    flags.authenticate_http_readwrite = false;
#endif // USE_SSL_SOCKET

    return flags;
  }
};


// These tests are parameterized by the containerizers enabled on the agent.
//
// TODO(gkleiman): The version of gtest currently used by Mesos doesn't support
// passing `::testing::Values` a single value. Update these calls once we
// upgrade to a newer version.
INSTANTIATE_TEST_CASE_P(
    MesosContainerizer,
    DefaultExecutorTest,
    ::testing::ValuesIn(vector<string>({"mesos"})));

INSTANTIATE_TEST_CASE_P(
    ROOT_DOCKER_DockerAndMesosContainerizers,
    DefaultExecutorTest,
    ::testing::ValuesIn(vector<string>({"docker,mesos"})));


// This test verifies that the default executor can launch a task group.
TEST_P(DefaultExecutorTest, TaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  EXPECT_EQ(taskInfo.task_id(), update->status().task_id());
  EXPECT_TRUE(update->status().has_timestamp());

  // Ensure that the task sandbox symbolic link is created.
  EXPECT_TRUE(os::exists(path::join(
      slave::paths::getExecutorLatestRunPath(
          flags.work_dir,
          devolve(agentId),
          devolve(frameworkId),
          devolve(executorInfo.executor_id())),
      "tasks",
      taskInfo.task_id().value())));

  // Verify that the executor's type is exposed in the agent's state
  // endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);
  JSON::Object state = parse.get();

  EXPECT_SOME_EQ(
      JSON::String(v1::ExecutorInfo::Type_Name(executorInfo.type())),
      state.find<JSON::String>("frameworks[0].executors[0].type"));
}


// This test verifies that if the default executor is asked
// to kill a task from a task group, it kills all tasks in
// the group and sends TASK_KILLED updates for them.
TEST_P(DefaultExecutorTest, KillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers1);
  EXPECT_NE(0, offers1->offers().size());

  const v1::Offer& offer1 = offers1->offers(0);
  const v1::AgentID& agentId = offer1.agent_id();

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskGroupInfo taskGroup1;
  taskGroup1.add_tasks()->CopyFrom(taskInfo1);
  taskGroup1.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks1{taskInfo1.task_id(), taskInfo2.task_id()};

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer1.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    // Set a 0s filter to immediately get another offer to launch
    // the second task group.
    accept->mutable_filters()->set_refuse_seconds(0);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup1);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(TASK_RUNNING, runningUpdate1->status().state());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(TASK_RUNNING, runningUpdate2->status().state());

  // When running a task, TASK_RUNNING updates for the tasks in a
  // task group can be received in any order.
  const hashset<v1::TaskID> tasksRunning{
    runningUpdate1->status().task_id(),
    runningUpdate2->status().task_id()};

  ASSERT_EQ(tasks1, tasksRunning);

  AWAIT_READY(offers2);
  const v1::Offer& offer2 = offers2->offers(0);

  v1::TaskInfo taskInfo3 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskGroupInfo taskGroup2;
  taskGroup2.add_tasks()->CopyFrom(taskInfo3);

  Future<v1::scheduler::Event::Update> runningUpdate3;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate3));

  // Launch the second task group.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer2.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup2);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate3);
  ASSERT_EQ(TASK_RUNNING, runningUpdate3->status().state());
  ASSERT_EQ(taskInfo3.task_id(), runningUpdate3->status().task_id());

  // Acknowledge the TASK_RUNNING updates to receive the next updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate1->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer1.agent_id());
    acknowledge->set_uuid(runningUpdate1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate2->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer1.agent_id());
    acknowledge->set_uuid(runningUpdate2->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate3->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer2.agent_id());
    acknowledge->set_uuid(runningUpdate3->status().uuid());

    mesos.send(call);
  }

  Future<v1::scheduler::Event::Update> killedUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate1))
    .WillOnce(FutureArg<1>(&killedUpdate2));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  // Now kill a task in the first task group.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo1.task_id());

    mesos.send(call);
  }

  // All the tasks in the first task group should be killed.

  AWAIT_READY(killedUpdate1);
  ASSERT_EQ(TASK_KILLED, killedUpdate1->status().state());

  AWAIT_READY(killedUpdate2);
  ASSERT_EQ(TASK_KILLED, killedUpdate2->status().state());

  // When killing a task, TASK_KILLED updates for the tasks in a task
  // group can be received in any order.
  const hashset<v1::TaskID> tasksKilled{
    killedUpdate1->status().task_id(),
    killedUpdate2->status().task_id()};

  ASSERT_EQ(tasks1, tasksKilled);

  // The executor should still be alive after the first task
  // group has been killed.
  ASSERT_TRUE(executorFailure.isPending());

  Future<v1::scheduler::Event::Update> killedUpdate3;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate3));

  // Now kill the only task present in the second task group.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo3.task_id());

    mesos.send(call);
  }

  AWAIT_READY(killedUpdate3);
  ASSERT_EQ(TASK_KILLED, killedUpdate3->status().state());
  ASSERT_EQ(taskInfo3.task_id(), killedUpdate3->status().task_id());

  // The executor should commit suicide after all the tasks have been
  // killed.
  AWAIT_READY(executorFailure);

  // Even though the tasks were killed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());
}


// This test verifies that if the default executor receives a
// non-zero exit status code for a task in the task group, it
// kills all the other tasks (default restart policy).
TEST_P(DefaultExecutorTest, KillTaskGroupOnTaskFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The first task exits with a non-zero status code.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 1");

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(TASK_RUNNING, runningUpdate1->status().state());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(TASK_RUNNING, runningUpdate2->status().state());

  // When running a task, TASK_RUNNING updates for the tasks in a task
  // group can be received in any order.
  const hashset<v1::TaskID> tasksRunning{
    runningUpdate1->status().task_id(),
    runningUpdate2->status().task_id()};

  ASSERT_EQ(tasks, tasksRunning);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  // Acknowledge the TASK_RUNNING updates to receive the next updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate1->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate2->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate2->status().uuid());

    mesos.send(call);
  }

  // Updates for the tasks in a task group can be received in any order.
  set<pair<v1::TaskID, v1::TaskState>> taskStates;

  taskStates.insert({taskInfo1.task_id(), v1::TASK_FAILED});
  taskStates.insert({taskInfo2.task_id(), v1::TASK_KILLED});

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  set<std::pair<v1::TaskID, v1::TaskState>> expectedTaskStates;

  expectedTaskStates.insert(
      {update1->status().task_id(), update1->status().state()});

  expectedTaskStates.insert(
      {update2->status().task_id(), update2->status().state()});

  ASSERT_EQ(expectedTaskStates, taskStates);
}


// Verifies that a task in a task group with an executor is accepted
// during `TaskGroupInfo` validation.
TEST_P(DefaultExecutorTest, TaskUsesExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  taskInfo.mutable_executor()->CopyFrom(executorInfo);

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  EXPECT_EQ(taskInfo.task_id(), update->status().task_id());
  EXPECT_TRUE(update->status().has_timestamp());
}


// This test verifies that the container status for a task in a task
// group is set properly. In other words, it is the status of the
// container that corresponds to the task.
TEST_P(DefaultExecutorTest, ROOT_ContainerStatusForTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(DoAll(
        v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO),
        FutureSatisfy(&connected)));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo task1 = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo(SLEEP_COMMAND(1000)));

  v1::TaskInfo task2 = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo(SLEEP_COMMAND(1000)));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({task1, task2}));

  Future<Event::Update> updateRunning1;
  Future<Event::Update> updateRunning2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&updateRunning1),
        v1::scheduler::SendAcknowledge(
            frameworkId,
            offer.agent_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&updateRunning2),
        v1::scheduler::SendAcknowledge(
            frameworkId,
            offer.agent_id())));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateRunning1);
  AWAIT_READY(updateRunning2);

  ASSERT_EQ(TASK_RUNNING, updateRunning1->status().state());
  ASSERT_EQ(TASK_RUNNING, updateRunning2->status().state());

  ASSERT_TRUE(updateRunning1->status().has_container_status());
  ASSERT_TRUE(updateRunning2->status().has_container_status());

  v1::ContainerStatus status1 = updateRunning1->status().container_status();
  v1::ContainerStatus status2 = updateRunning2->status().container_status();

  ASSERT_TRUE(status1.has_container_id());
  ASSERT_TRUE(status2.has_container_id());

  EXPECT_TRUE(status1.container_id().has_parent());
  EXPECT_TRUE(status2.container_id().has_parent());
  EXPECT_NE(status1.container_id(), status2.container_id());
  EXPECT_EQ(status1.container_id().parent(),
            status2.container_id().parent());
}


// This test verifies that the default executor commits suicide when the only
// task in the task group exits with a non-zero status code.
TEST_P(DefaultExecutorTest, CommitSuicideOnTaskFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The task exits with a non-zero status code.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 1");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);

  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> failedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate))
    .WillOnce(FutureArg<1>(&failedUpdate));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());

  // Acknowledge the TASK_RUNNING update to receive the next update.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(failedUpdate);
  ASSERT_EQ(TASK_FAILED, failedUpdate->status().state());

  // The executor should commit suicide when the task exits with
  // a non-zero status code.
  AWAIT_READY(executorFailure);

  // Even though the task failed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());
}


// This test verifies that the default executor does not commit suicide
// with a non-zero exit code after killing a task from a task group when
// one of its tasks finished successfully earlier (See MESOS-7129).
TEST_P(DefaultExecutorTest, CommitSuicideOnKillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The first task finishes successfully while the second
  // task is explicitly killed later.

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 0");

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(TASK_RUNNING, runningUpdate1->status().state());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(TASK_RUNNING, runningUpdate2->status().state());

  // When running a task, TASK_RUNNING updates for the tasks in a
  // task group can be received in any order.
  const hashset<v1::TaskID> tasksRunning{
    runningUpdate1->status().task_id(),
    runningUpdate2->status().task_id()};

  ASSERT_EQ(tasks, tasksRunning);

  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&finishedUpdate));

  // Acknowledge the TASK_RUNNING updates to receive the next updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate1->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();

    acknowledge->mutable_task_id()->CopyFrom(
        runningUpdate2->status().task_id());

    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate2->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo1.task_id(), finishedUpdate->status().task_id());

  // The executor should still be alive after the task
  // has finished successfully.
  ASSERT_TRUE(executorFailure.isPending());

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate));

  // Now kill the second task in the task group.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo2.task_id());

    mesos.send(call);
  }

  AWAIT_READY(killedUpdate);
  ASSERT_EQ(TASK_KILLED, killedUpdate->status().state());
  ASSERT_EQ(taskInfo2.task_id(), killedUpdate->status().task_id());

  // The executor should commit suicide after the task is killed.
  AWAIT_READY(executorFailure);

  // Even though the task failed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());
}


// This test verifies that the default executor can be
// launched using reserved resources.
TEST_P(DefaultExecutorTest, ReservedResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  // Launch the executor using reserved resources.
  v1::Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      v1::createReservationInfo(frameworkInfo.principal())).get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(reserved);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Launch the task using unreserved resources.
  v1::TaskInfo taskInfo =
    v1::createTask(agentId, unreserved, SLEEP_COMMAND(1000));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    accept->add_operations()->CopyFrom(v1::RESERVE(reserved));

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
}


struct LauncherAndIsolationParam
{
  LauncherAndIsolationParam(const string& _launcher, const string& _isolation)
    : launcher(_launcher), isolation(_isolation) {}

  const string launcher;
  const string isolation;
};


class PersistentVolumeDefaultExecutor
  : public MesosTest,
    public WithParamInterface<LauncherAndIsolationParam>
{
public:
  PersistentVolumeDefaultExecutor() : param(GetParam()) {}

protected:
  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifndef USE_SSL_SOCKET
    // Disable operator API authentication for the default executor. Executor
    // authentication currently has SSL as a dependency, so we cannot require
    // executors to authenticate with the agent operator API if Mesos was not
    // built with SSL support.
    flags.authenticate_http_readwrite = false;
#endif // USE_SSL_SOCKET

    return flags;
  }

  LauncherAndIsolationParam param;
};


INSTANTIATE_TEST_CASE_P(
    LauncherAndIsolationParam,
    PersistentVolumeDefaultExecutor,
    ::testing::Values(
        LauncherAndIsolationParam("posix", "volume/sandbox_path"),
        LauncherAndIsolationParam("linux", "volume/sandbox_path"),
        LauncherAndIsolationParam(
            "linux",
            "filesystem/linux,volume/sandbox_path")));


// This test verifies that the default executor can be launched using
// reserved persistent resources which can be accessed by its tasks.
TEST_P(PersistentVolumeDefaultExecutor, ROOT_PersistentResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = param.launcher;
  flags.isolation = param.isolation;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(DoAll(v1::scheduler::SendSubscribe(frameworkInfo),
                    FutureSatisfy(&connected)));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      v1::createReservationInfo(frameworkInfo.principal())).get();

  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.role(),
      "id1",
      "executor_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources executorResources = reserved.apply(v1::CREATE(volume)).get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID.value(),
      None(),
      None(),
      v1::ExecutorInfo::DEFAULT);

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(executorResources);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);

  // Launch a task that accesses executor's volume.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      unreserved,
      "echo abc > task_volume_path/file");

  // TODO(gilbert): Refactor the following code once the helper
  // to create a 'sandbox_path' volume is suppported.
  mesos::v1::ContainerInfo* containerInfo = taskInfo.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);

  mesos::v1::Volume* taskVolume = containerInfo->add_volumes();
  taskVolume->set_mode(mesos::v1::Volume::RW);
  taskVolume->set_container_path("task_volume_path");

  mesos::v1::Volume::Source* source = taskVolume->mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path");

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(FutureArg<1>(&updateFinished));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateRunning);
  ASSERT_EQ(TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(TASK_FINISHED, updateFinished->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFinished->status().task_id());

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      devolve(volume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}


// This test verifies that the default executor mounts the persistent volume
// in the task container when it is set on a task in the task group.
TEST_P(PersistentVolumeDefaultExecutor, ROOT_TaskSandboxPersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = param.launcher;
  flags.isolation = param.isolation;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(DoAll(v1::scheduler::SendSubscribe(frameworkInfo),
                    FutureSatisfy(&connected)));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID.value(),
      None(),
      None(),
      v1::ExecutorInfo::DEFAULT);

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(unreserved);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);

  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.role(),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources reserved = unreserved.flatten(
      frameworkInfo.role(),
      v1::createReservationInfo(frameworkInfo.principal())).get();

  // Launch a task that expects the persistent volume to be
  // mounted in its sandbox.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      reserved.apply(v1::CREATE(volume)).get(),
      "echo abc > task_volume_path/file");

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(FutureArg<1>(&updateFinished));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateRunning);
  ASSERT_EQ(TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(TASK_FINISHED, updateFinished->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFinished->status().task_id());

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      devolve(volume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
