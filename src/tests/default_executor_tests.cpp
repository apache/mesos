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
#include <stout/os/killtree.hpp>

#include "slave/paths.hpp"

#include "tests/cluster.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

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

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::slave::ContainerTermination;

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
INSTANTIATE_TEST_CASE_P(
    MesosContainerizer,
    DefaultExecutorTest,
    ::testing::Values("mesos"));

INSTANTIATE_TEST_CASE_P(
    ROOT_DOCKER_DockerAndMesosContainerizers,
    DefaultExecutorTest,
    ::testing::Values("docker,mesos"));


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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer1 = offers1->offers(0);
  const v1::AgentID& agentId = offer1.agent_id();

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  const hashset<v1::TaskID> tasks1{taskInfo1.task_id(), taskInfo2.task_id()};

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());

  {
    v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
        executorInfo,
        v1::createTaskGroupInfo({taskInfo1, taskInfo2}));
    Call call = v1::createCallAccept(frameworkId, offer1, {launchGroup});

    // Set a 0s filter to immediately get another offer to launch
    // the second task group.
    call.mutable_accept()->mutable_filters()->set_refuse_seconds(0);

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

  Future<v1::scheduler::Event::Update> runningUpdate3;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, offer2.agent_id())));

  // Launch the second task group.
  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer2,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo3}))}));

  AWAIT_READY(runningUpdate3);
  ASSERT_EQ(TASK_RUNNING, runningUpdate3->status().state());
  ASSERT_EQ(taskInfo3.task_id(), runningUpdate3->status().task_id());

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The first task exits with a non-zero status code.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 1");

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  taskInfo.mutable_executor()->CopyFrom(executorInfo);

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo task1 = v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo task2 = v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<Event::Update> updateRunning1;
  Future<Event::Update> updateRunning2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&updateRunning1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&updateRunning2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({task1, task2}))}));

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The task exits with a non-zero status code.
  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, "exit 1");

  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> failedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(FutureArg<1>(&failedUpdate));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());

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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The first task finishes successfully while the second
  // task is explicitly killed later.

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 0");

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

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

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role(DEFAULT_TEST_ROLE);

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

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  // Launch the executor using reserved resources.
  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.role(), frameworkInfo.principal()));

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      reserved,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  // Launch the task using unreserved resources.
  v1::TaskInfo taskInfo =
    v1::createTask(agentId, unreserved, SLEEP_COMMAND(1000));

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation launchGroup =
    v1::LAUNCH_GROUP(executorInfo, v1::createTaskGroupInfo({taskInfo}));

  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate));

  mesos.send(v1::createCallAccept(frameworkId, offer, {reserve, launchGroup}));

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
}


// This is a regression test for MESOS-7926. It verifies that if
// the default executor process is killed, the future of the nested
// container destroy will be discarded and that discard will
// not propagate back to the executor container destroy, to make
// sure the executor container destroy can be finished correctly.
TEST_P(DefaultExecutorTest, SigkillExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());
  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "sleep 1000");

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  EXPECT_EQ(taskInfo.task_id(), update->status().task_id());
  EXPECT_TRUE(update->status().has_timestamp());
  ASSERT_TRUE(update->status().has_container_status());

  v1::ContainerStatus status = update->status().container_status();

  ASSERT_TRUE(status.has_container_id());
  EXPECT_TRUE(status.container_id().has_parent());

  v1::ContainerID executorContainerId = status.container_id().parent();

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(devolve(executorContainerId));

  Future<ContainerStatus> executorStatus =
    containerizer->status(devolve(executorContainerId));

  AWAIT_READY(executorStatus);
  ASSERT_TRUE(executorStatus->has_executor_pid());

  ASSERT_SOME(os::killtree(executorStatus->executor_pid(), SIGKILL));

  // In this test we do not care about the exact value of the returned status
  // code, but ensure that `wait` future enters the ready state.
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
}


#ifdef __linux__
// This test verifies that tasks from two different
// task groups can share the same pid namespace.
TEST_P(DefaultExecutorTest, ROOT_MultiTaskgroupSharePidNamespace)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());
  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer1 = offers1->offers(0);
  const v1::AgentID& agentId = offer1.agent_id();

  // Create the first task which will share pid namespace with its parent.
  v1::TaskInfo taskInfo1 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "stat -Lc %i /proc/self/ns/pid > ns && sleep 1000");

  mesos::v1::ContainerInfo* containerInfo = taskInfo1.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo->mutable_linux_info()->set_share_pid_namespace(true);

  Future<v1::scheduler::Event::Update> update1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1));

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());

  // Launch the first task group.
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo1}));

  mesos.send(v1::createCallAccept(frameworkId, offer1, {launchGroup}));

  AWAIT_READY(update1);

  ASSERT_EQ(TASK_RUNNING, update1->status().state());
  EXPECT_EQ(taskInfo1.task_id(), update1->status().task_id());
  EXPECT_TRUE(update1->status().has_timestamp());

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  // Create the second task which will share pid namespace with its parent.
  v1::TaskInfo taskInfo2 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "stat -Lc %i /proc/self/ns/pid > ns && sleep 1000");

  containerInfo = taskInfo2.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo->mutable_linux_info()->set_share_pid_namespace(true);

  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update2));

  // Launch the second task group.
  launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo2}));

  mesos.send(v1::createCallAccept(frameworkId, offer2, {launchGroup}));

  AWAIT_READY(update2);

  ASSERT_EQ(TASK_RUNNING, update2->status().state());
  EXPECT_EQ(taskInfo2.task_id(), update2->status().task_id());
  EXPECT_TRUE(update2->status().has_timestamp());

  string executorSandbox = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  string pidNamespacePath1 = path::join(
      executorSandbox,
      "tasks",
      taskInfo1.task_id().value(),
      "ns");

  string pidNamespacePath2 = path::join(
      executorSandbox,
      "tasks",
      taskInfo2.task_id().value(),
      "ns");

  // Wait up to 5 seconds for each of the two tasks to
  // write its pid namespace inode into its sandbox.
  Duration waited = Duration::zero();
  do {
    if (os::exists(pidNamespacePath1) && os::exists(pidNamespacePath2)) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < Seconds(5));

  EXPECT_TRUE(os::exists(pidNamespacePath1));
  EXPECT_TRUE(os::exists(pidNamespacePath2));

  Try<string> pidNamespace1 = os::read(pidNamespacePath1);
  ASSERT_SOME(pidNamespace1);

  Try<string> pidNamespace2 = os::read(pidNamespacePath2);
  ASSERT_SOME(pidNamespace2);

  // Check the two tasks share the same pid namespace.
  EXPECT_EQ(strings::trim(pidNamespace1.get()),
            strings::trim(pidNamespace2.get()));
}
#endif // __linux__


struct LauncherAndIsolationParam
{
  LauncherAndIsolationParam(const string& _launcher, const string& _isolation)
    : launcher(_launcher), isolation(_isolation) {}

  const string launcher;
  const string isolation;
};


// This test verifies that URIs set on tasks are fetched and made available to
// them when started by the DefaultExecutor.
//
// TODO(josephw): Reenable this test once URIs are constructed without using
// the `path` helpers. This should be fixed along with MESOS-6705.
TEST_P_TEMP_DISABLED_ON_WINDOWS(DefaultExecutorTest, TaskWithFileURI)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

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

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  // Create a task that will check if a file called 'testFile'
  // contains the text 'pizza'.

  // Create the file that should be fetched for the task
  string fromPath = path::join(os::getcwd(), "fromPath");
  ASSERT_SOME(os::mkdir(fromPath));
  string testFilePath = path::join(fromPath, "testFile");
  EXPECT_SOME(os::write(testFilePath, "pizza"));

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "test `cat testFile` = pizza");

  taskInfo.mutable_command()->add_uris()->set_value("file://" + testFilePath);

  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}


// This test verifies that URIs set on Docker tasks are fetched and made
// available to them when started by the DefaultExecutor.
//
// TODO(josephw): Reenable this test once URIs are constructed without using
// the `path` helpers. This should be fixed along with MESOS-6705.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    DefaultExecutorTest, ROOT_INTERNET_CURL_DockerTaskWithFileURI)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

  // Image pulling time may be long, depending on the location of
  // the registry server.
  flags.executor_registration_timeout = Minutes(10);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

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

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  // Create a task that will check if a file called 'testFile'
  // contains the text 'pizza'.

  // Create the file that should be fetched for the task
  string fromPath = path::join(os::getcwd(), "fromPath");
  ASSERT_SOME(os::mkdir(fromPath));
  string testFilePath = path::join(fromPath, "testFile");
  EXPECT_SOME(os::write(testFilePath, "pizza"));

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "test `cat testFile` = pizza");

  taskInfo.mutable_command()->add_uris()->set_value("file://" + testFilePath);

  mesos::v1::Image image;
  image.set_type(mesos::v1::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  mesos::v1::ContainerInfo* container = taskInfo.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}


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
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    PersistentVolumeDefaultExecutor, ROOT_PersistentResources)
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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.role(), frameworkInfo.principal()));

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
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      executorResources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Launch a task that accesses executor's volume.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      unreserved,
      "echo abc > task_volume_path/file");

  // TODO(gilbert): Refactor the following code once the helper
  // to create a 'sandbox_path' volume is supported.
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
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    PersistentVolumeDefaultExecutor, ROOT_TaskSandboxPersistentVolume)
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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources unreserved =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      unreserved,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.role(),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.role(), frameworkInfo.principal()));

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


// This test verifies that sibling tasks in the same task group can share a
// Volume owned by their parent executor using 'sandbox_path' volumes.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    PersistentVolumeDefaultExecutor, ROOT_TasksSharingViaSandboxVolumes)
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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources individualResources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get()
      .pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.role(), frameworkInfo.principal()));

  v1::Resources totalResources =
    v1::Resources::parse("cpus:0.3;mem:96;disk:96").get()
      .pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.role(), frameworkInfo.principal()));

  v1::Resource executorVolume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.role(),
      "executor",
      "executor_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources executorResources =
    individualResources.apply(v1::CREATE(executorVolume)).get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      executorResources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Create a "producer" task that creates a file in a 'sandbox_path' Volume
  // owned by the Executor, and a "consumer" task that waits for the file to
  // exist in a 'sandbox_path' Volume owned by the Executor.
  //
  // The test will only succeed if the task volume's source path is set to the
  // path of the executor's persistent volume.

  // TODO(gilbert): Refactor the following code once the helper to create a
  // 'sandbox_path' volume is supported.

  mesos::v1::Volume taskVolume;
  taskVolume.set_mode(mesos::v1::Volume::RW);
  taskVolume.set_container_path("task_volume_path");

  mesos::v1::Volume::Source* source = taskVolume.mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path");

  mesos::v1::ContainerInfo containerInfo;
  containerInfo.set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(taskVolume);

  // A "producer" task that expects the persistent volume to be mounted in its
  // sandbox.
  v1::TaskInfo producerInfo = v1::createTask(
      offer.agent_id(),
      individualResources,
      "echo abc > task_volume_path/file",
      None(),
      "producer",
      "producer");
  producerInfo.mutable_container()->CopyFrom(containerInfo);

  // A "consumer" task that expects the persistent volume to be mounted in its
  // sandbox, and waits for a file to exist before exiting.
  v1::TaskInfo consumerInfo = v1::createTask(
      offer.agent_id(),
      individualResources,
      "while [ ! -f task_volume_path/file ]; do sleep 1; done\ntrue",
      None(),
      "consumer",
      "consumer");
  consumerInfo.mutable_container()->CopyFrom(containerInfo);

  vector<Future<v1::scheduler::Event::Update>> updates(4);

  {
    // This variable doesn't have to be used explicitly. We need it so that the
    // futures are satisfied in the order in which the updates are received.
    testing::InSequence inSequence;

    foreach (Future<v1::scheduler::Event::Update>& update, updates) {
      EXPECT_CALL(*scheduler, update(_, _))
        .WillOnce(
            DoAll(
                FutureArg<1>(&update),
                v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())));
    }
  }

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::RESERVE(totalResources),
           v1::CREATE(executorVolume),
           v1::LAUNCH_GROUP(
               executorInfo,
               v1::createTaskGroupInfo({producerInfo, consumerInfo}))}));

  // We track the status updates of each task separately to verify that they
  // transition from TASK_RUNNING to TASK_FINISHED.

  enum class Stage
  {
    INITIAL,
    RUNNING,
    FINISHED
  };

  hashmap<v1::TaskID, Stage> taskStages;
  taskStages[producerInfo.task_id()] = Stage::INITIAL;
  taskStages[consumerInfo.task_id()] = Stage::INITIAL;

  foreach (Future<v1::scheduler::Event::Update>& update, updates) {
    AWAIT_READY(update);

    const v1::TaskStatus& taskStatus = update->status();

    Option<Stage> taskStage = taskStages.get(taskStatus.task_id());
    ASSERT_SOME(taskStage);

    switch (taskStage.get()) {
      case Stage::INITIAL: {
        ASSERT_EQ(TASK_RUNNING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::RUNNING;

        break;
      }
      case Stage::RUNNING: {
        ASSERT_EQ(TASK_FINISHED, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::FINISHED;

        break;
      }
      case Stage::FINISHED: {
        FAIL() << "Unexpected task update: " << update->DebugString();
        break;
      }
    }
  }

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir, devolve(executorVolume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}


// This test verifies that sibling tasks in different task groups can share a
// Volume owned by their parent executor using 'sandbox_path' volumes.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    PersistentVolumeDefaultExecutor, ROOT_TaskGroupsSharingViaSandboxVolumes)
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

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources individualResources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get()
      .pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.role(), frameworkInfo.principal()));

  v1::Resources totalResources =
    v1::Resources::parse("cpus:0.3;mem:96;disk:96").get()
      .pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.role(), frameworkInfo.principal()));

  v1::Resource executorVolume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.role(),
      "executor",
      "executor_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources executorResources =
    individualResources.apply(v1::CREATE(executorVolume)).get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      executorResources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Create a "producer" task that creates a file in a 'sandbox_path' Volume
  // owned by the Executor, and a "consumer" task that waits for the file to
  // exist in a 'sandbox_path' Volume owned by the Executor.
  //
  // The test will only succeed if the task volume's source path is set to the
  // path of the executor's persistent volume.

  // TODO(gilbert): Refactor the following code once the helper to create a
  // 'sandbox_path' volume is supported.

  mesos::v1::Volume taskVolume;
  taskVolume.set_mode(mesos::v1::Volume::RW);
  taskVolume.set_container_path("task_volume_path");

  mesos::v1::Volume::Source* source = taskVolume.mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path");

  mesos::v1::ContainerInfo containerInfo;
  containerInfo.set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(taskVolume);

  // A "producer" task that expects the persistent volume to be mounted in its
  // sandbox.
  v1::TaskInfo producerInfo = v1::createTask(
      offer.agent_id(),
      individualResources,
      "echo abc > task_volume_path/file",
      None(),
      "producer",
      "producer");
  producerInfo.mutable_container()->CopyFrom(containerInfo);

  // A "consumer" task that expects the persistent volume to be mounted in its
  // sandbox, and waits for a file to exist before exiting.
  v1::TaskInfo consumerInfo = v1::createTask(
      offer.agent_id(),
      individualResources,
      "while [ ! -f task_volume_path/file ]; do sleep 1; done\ntrue",
      None(),
      "consumer",
      "consumer");
  consumerInfo.mutable_container()->CopyFrom(containerInfo);

  vector<Future<v1::scheduler::Event::Update>> updates(4);

  {
    // This variable doesn't have to be used explicitly. We need it so that the
    // futures are satisfied in the order in which the updates are received.
    testing::InSequence inSequence;

    foreach (Future<v1::scheduler::Event::Update>& update, updates) {
      EXPECT_CALL(*scheduler, update(_, _))
        .WillOnce(
            DoAll(
                FutureArg<1>(&update),
                v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())));
    }
  }

  // Reserve the resources, create the Executor's volume, and launch each task
  // in a different task group.
  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::RESERVE(totalResources),
           v1::CREATE(executorVolume),
           v1::LAUNCH_GROUP(
               executorInfo, v1::createTaskGroupInfo({producerInfo})),
           v1::LAUNCH_GROUP(
               executorInfo, v1::createTaskGroupInfo({consumerInfo}))}));

  // We track the status updates of each task separately to verify that they
  // transition from TASK_RUNNING to TASK_FINISHED.

  enum class Stage
  {
    INITIAL,
    RUNNING,
    FINISHED
  };

  hashmap<v1::TaskID, Stage> taskStages;
  taskStages[producerInfo.task_id()] = Stage::INITIAL;
  taskStages[consumerInfo.task_id()] = Stage::INITIAL;

  foreach (Future<v1::scheduler::Event::Update>& update, updates) {
    AWAIT_READY(update);

    const v1::TaskStatus& taskStatus = update->status();

    Option<Stage> taskStage = taskStages.get(taskStatus.task_id());
    ASSERT_SOME(taskStage);

    switch (taskStage.get()) {
      case Stage::INITIAL: {
        ASSERT_EQ(TASK_RUNNING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::RUNNING;

        break;
      }
      case Stage::RUNNING: {
        ASSERT_EQ(TASK_FINISHED, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::FINISHED;

        break;
      }
      case Stage::FINISHED: {
        FAIL() << "Unexpected task update: " << update->DebugString();
        break;
      }
    }
  }

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir, devolve(executorVolume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
