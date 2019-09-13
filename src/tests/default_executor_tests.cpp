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
#include <stout/uri.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/killtree.hpp>

#include "slave/paths.hpp"

#include "tests/cluster.hpp"
#include "tests/containerizer.hpp"
#include "tests/kill_policy_test_helper.hpp"
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
using testing::AllOf;
using testing::DoAll;
using testing::Return;
using testing::WithParamInterface;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::slave::ContainerTermination;

#ifndef __WINDOWS__
namespace process {

void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readonlyAuthenticationRealm,
    const Option<string>& readwriteAuthenticationRealm);

} // namespace process {
#endif // __WINDOWS__


namespace mesos {
namespace internal {
namespace tests {

// Tests that exercise the default executor implementation
// should be located in this file.

class DefaultExecutorTest
  : public MesosTest,
    public WithParamInterface<string> {};


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

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(FutureArg<1>(&runningUpdate))
    .WillRepeatedly(Return());

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);

  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());
  EXPECT_TRUE(startingUpdate->status().has_timestamp());

  AWAIT_READY(runningUpdate);

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


// This is a regression test for MESOS-9332. It launches a nested container as
// a non-root user which is inherited from its parent container, and verifies
// that the nested container's user is same as the owner of its sandbox.
TEST_P(DefaultExecutorTest, ROOT_UNPRIVILEGED_USER_SandboxOwnership)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  // Set the framework user to a non-root user so the default executor will
  // also be run as this non-root user since the default executor always runs
  // as the same user of the framework.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_user(user.get());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Launch a task without specifying user in its `commandInfo` so the
  // corresponding nested container will inherit user from its parent
  // container (i.e., the default executor). The task will run a command
  // to verify its user is same as the owner of its sandbox.
  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
      "test `stat -c \"%U\" .` = " + user.get());

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(v1::TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
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

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> killedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate2),
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

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);

  AWAIT_READY(offers2);
  const v1::Offer& offer2 = offers2->offers(0);

  v1::TaskInfo taskInfo3 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<v1::scheduler::Event::Update> startingUpdate3;
  Future<v1::scheduler::Event::Update> runningUpdate3;
  Future<v1::scheduler::Event::Update> killedUpdate3;

  testing::Sequence task3;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Launch the second task group.
  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer2,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo3}))}));

  AWAIT_READY(startingUpdate3);
  AWAIT_READY(runningUpdate3);

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  ASSERT_TRUE(killedUpdate1.isPending());
  ASSERT_TRUE(killedUpdate2.isPending());
  ASSERT_TRUE(killedUpdate3.isPending());

  // Now kill a task in the first task group.
  mesos.send(v1::createCallKill(frameworkId, taskInfo1.task_id()));

  // Only the tasks in the first group were killed.
  AWAIT_READY(killedUpdate1);
  AWAIT_READY(killedUpdate2);
  ASSERT_TRUE(killedUpdate3.isPending());

  // The executor should still be alive after the first task
  // group has been killed.
  ASSERT_TRUE(executorFailure.isPending());

  // Now kill the only task present in the second task group.
  mesos.send(v1::createCallKill(frameworkId, taskInfo3.task_id()));

  AWAIT_READY(killedUpdate3);

  // The executor should commit suicide after all the tasks have been
  // killed.
  AWAIT_READY(executorFailure);

  // Even though the tasks were killed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());
}


// This is a regression test for MESOS-8051. It verifies that if the
// default executor is asked to kill all tasks from a task group
// simultaneously, all the tasks can be successfully killed and the
// default executor can send TASK_KILLED updates for all of them.
TEST_P(DefaultExecutorTest, KillMultipleTasks)
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

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> killedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);

  // Now kill all tasks in the task group.
  mesos.send(v1::createCallKill(frameworkId, taskInfo1.task_id()));
  mesos.send(v1::createCallKill(frameworkId, taskInfo2.task_id()));

  // All the tasks in the task group should be killed.
  AWAIT_READY(killedUpdate1);
  AWAIT_READY(killedUpdate2);
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

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> failedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FAILED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&failedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> killedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);
  AWAIT_READY(failedUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);
  AWAIT_READY(killedUpdate2);
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

  ASSERT_EQ(v1::TASK_STARTING, update->status().state());
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

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);

  ASSERT_TRUE(runningUpdate1->status().has_container_status());
  ASSERT_TRUE(runningUpdate2->status().has_container_status());

  v1::ContainerStatus status1 = runningUpdate1->status().container_status();
  v1::ContainerStatus status2 = runningUpdate2->status().container_status();

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

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> failedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
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

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());

  AWAIT_READY(failedUpdate);
  ASSERT_EQ(v1::TASK_FAILED, failedUpdate->status().state());

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

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> finishedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> killedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate2),
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

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);
  AWAIT_READY(finishedUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);

  ASSERT_TRUE(killedUpdate2.isPending());

  // The executor should still be alive after task1 has finished successfully.
  ASSERT_TRUE(executorFailure.isPending());

  // Now kill the second task in the task group.
  mesos.send(v1::createCallKill(frameworkId, taskInfo2.task_id()));

  AWAIT_READY(killedUpdate2);

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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
        frameworkInfo.roles(0), frameworkInfo.principal()));

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

  Future<v1::scheduler::Event::Update> startingUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&startingUpdate));

  mesos.send(v1::createCallAccept(frameworkId, offer, {reserve, launchGroup}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());
}


// This test verifies that the agent could recover if the agent
// metadata is checkpointed.
TEST_P(DefaultExecutorTest, SlaveRecoveryWithMetadataCheckpointed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo.set_checkpoint(true);

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

  v1::Offer::Operation launchGroup =
    v1::LAUNCH_GROUP(executorInfo, v1::createTaskGroupInfo({taskInfo}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return()); // Ignore subsequent status updates.

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
  EXPECT_TRUE(runningUpdate->status().has_timestamp());
  ASSERT_TRUE(runningUpdate->status().has_container_status());

  slave.get()->terminate();
  slave->reset();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  slave = this->StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);
}


#ifdef __linux__
// This test verifies that the agent could recover if the agent
// metadata is not checkpointed. This is a regression test for
// MESOS-8416.
//
// TODO(gilbert): For now, the test is linux specific because
// the posix launcher is not able to destroy orphan containers
// after recovery. Remove the `#ifdef __linux__` once MESOS-8771
// is fixed.
TEST_P(DefaultExecutorTest, ROOT_SlaveRecoveryWithoutMetadataCheckpointed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo.set_checkpoint(false);

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

  v1::Offer::Operation launchGroup =
    v1::LAUNCH_GROUP(executorInfo, v1::createTaskGroupInfo({taskInfo}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return()); // Ignore subsequent status updates.

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
  EXPECT_TRUE(runningUpdate->status().has_timestamp());
  ASSERT_TRUE(runningUpdate->status().has_container_status());

  slave.get()->terminate();
  slave->reset();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  slave = this->StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover);
}
#endif // __linux__


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
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore further offers.

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

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return());

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(startingUpdate);

  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);

  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
  EXPECT_TRUE(runningUpdate->status().has_timestamp());
  ASSERT_TRUE(runningUpdate->status().has_container_status());

  v1::ContainerStatus status = runningUpdate->status().container_status();

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


// This test verifies that the default executor terminates the entire group
// when some task exceeded its `max_completion_time`.
TEST_P(DefaultExecutorTest, MaxCompletionTime)
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

  // Task 1 will finish before its max_completion_time.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "exit 0");

  taskInfo1.mutable_max_completion_time()->set_nanoseconds(Seconds(2).ns());

  // Task 2 will trigger its max_completion_time.
  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  taskInfo2.mutable_max_completion_time()->set_nanoseconds(Seconds(2).ns());

  // Task 3 has no max_completion_time.
  v1::TaskInfo taskInfo3 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> finishedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> failedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FAILED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&failedUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate3;
  Future<v1::scheduler::Event::Update> runningUpdate3;
  Future<v1::scheduler::Event::Update> killedUpdate3;

  testing::Sequence task3;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo(
                  {taskInfo1, taskInfo2, taskInfo3}))}));

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);
  AWAIT_READY(finishedUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);
  AWAIT_READY(failedUpdate2);

  EXPECT_EQ(
      v1::TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED,
      failedUpdate2->status().reason());

  AWAIT_READY(startingUpdate3);
  AWAIT_READY(runningUpdate3);
  AWAIT_READY(killedUpdate3);

  EXPECT_NE(
      v1::TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED,
      killedUpdate3->status().reason());

  // The executor should commit suicide after the task is killed.
  AWAIT_READY(executorFailure);

  // Even though the task failed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());
}

// TODO(qianzhang): Kill policy helpers are not yet enabled on Windows. See
// MESOS-8168.
#ifndef __WINDOWS__
// This test verifies that a task will transition from `TASK_KILLING`
// to `TASK_KILLED` rather than `TASK_FINISHED` when it is killed,
// even if it returns an "EXIT_STATUS" of 0 on receiving a SIGTERM.
TEST_P(DefaultExecutorTest, ROOT_NoTransitionFromKillingToFinished)
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

  // Start the framework with the task killing capability.
  v1::FrameworkInfo::Capability capability;
  capability.set_type(v1::FrameworkInfo::Capability::TASK_KILLING_STATE);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

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

  v1::CommandInfo commandInfo;
  commandInfo.set_shell(false);
  commandInfo.set_value(getTestHelperPath("test-helper"));
  commandInfo.add_arguments("test-helper");
  commandInfo.add_arguments(KillPolicyTestHelper::NAME);
  commandInfo.add_arguments("--sleep_duration=0");

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      commandInfo);

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY_FOR(startingUpdate, Seconds(60));
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());

  AWAIT_READY_FOR(runningUpdate, Seconds(60));
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  v1::ContainerStatus status = runningUpdate->status().container_status();

  ASSERT_TRUE(status.has_container_id());
  EXPECT_TRUE(status.container_id().has_parent());

  v1::ContainerID executorContainerId = status.container_id().parent();

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(devolve(executorContainerId));

  string executorSandbox = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  string filePath = path::join(
      executorSandbox,
      "tasks",
      taskInfo.task_id().value(),
      KillPolicyTestHelper::NAME);

  // Wait up to 5 seconds for the `test-helper` program to create a file into
  // its sandbox which is a signal that the task has been fully started.
  Duration waited = Duration::zero();
  do {
    if (os::exists(filePath)) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < Seconds(5));

  EXPECT_TRUE(os::exists(filePath));

  Future<v1::scheduler::Event::Update> killingUpdate;
  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&killingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate),
              v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Now kill the task in the task group, the default executor will
  // call the agent to send SIGTERM to the container. The `test-helper`
  // program will return an "EXIT_STATUS" of 0 on receiving a SIGTERM.
  mesos.send(v1::createCallKill(frameworkId, taskInfo.task_id()));

  AWAIT_READY(killingUpdate);
  ASSERT_EQ(v1::TASK_KILLING, killingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), killingUpdate->status().task_id());

  AWAIT_READY(killedUpdate);
  ASSERT_EQ(v1::TASK_KILLED, killedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), killedUpdate->status().task_id());

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  ASSERT_EQ(0, wait.get()->status());
}
#endif // __WINDOWS__


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
      "stat -Lc %i /proc/1/ns/pid > ns && sleep 1000");

  mesos::v1::ContainerInfo* containerInfo = taskInfo1.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo->mutable_linux_info()->set_share_pid_namespace(true);

  Future<v1::scheduler::Event::Update> update0;
  Future<v1::scheduler::Event::Update> update1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&update0),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
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

  ASSERT_EQ(v1::TASK_RUNNING, update1->status().state());
  EXPECT_EQ(taskInfo1.task_id(), update1->status().task_id());
  EXPECT_TRUE(update1->status().has_timestamp());

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  // Create the second task which will share pid namespace with its parent.
  v1::TaskInfo taskInfo2 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "stat -Lc %i /proc/1/ns/pid > ns && sleep 1000");

  containerInfo = taskInfo2.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);
  containerInfo->mutable_linux_info()->set_share_pid_namespace(true);

  Future<v1::scheduler::Event::Update> update2;
  Future<v1::scheduler::Event::Update> update3;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&update2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(FutureArg<1>(&update3));

  // Launch the second task group.
  launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo2}));

  mesos.send(v1::createCallAccept(frameworkId, offer2, {launchGroup}));

  AWAIT_READY(update2);

  ASSERT_EQ(v1::TASK_STARTING, update2->status().state());
  EXPECT_EQ(taskInfo2.task_id(), update2->status().task_id());

  AWAIT_READY(update3);

  ASSERT_EQ(v1::TASK_RUNNING, update3->status().state());
  EXPECT_EQ(taskInfo2.task_id(), update3->status().task_id());
  EXPECT_TRUE(update3->status().has_timestamp());

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

  // Wait up to 10 seconds for each of the two tasks to
  // write its pid namespace inode into its sandbox.
  Option<string> pidNamespace1;
  Option<string> pidNamespace2;

  Duration waited = Duration::zero();
  do {
    if (os::exists(pidNamespacePath1) && os::exists(pidNamespacePath2)) {
      Try<string> pidNamespace = os::read(pidNamespacePath1);
      ASSERT_SOME(pidNamespace);

      pidNamespace1 = pidNamespace.get();

      pidNamespace = os::read(pidNamespacePath2);
      ASSERT_SOME(pidNamespace);

      pidNamespace2 = pidNamespace.get();

      // It is possible that the `ns` file has been created but not written
      // yet (i.e., an empty file). To avoid comparing empty file, we will
      // only break from this loop when both task's `ns` files are not empty.
      if (!(strings::trim(pidNamespace1.get()).empty()) &&
          !(strings::trim(pidNamespace2.get()).empty())) {
        break;
      }
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < Seconds(10));

  ASSERT_SOME(pidNamespace1);
  ASSERT_SOME(pidNamespace2);

  // Check the two tasks share the same pid namespace.
  EXPECT_EQ(strings::trim(pidNamespace1.get()),
            strings::trim(pidNamespace2.get()));
}
#endif // __linux__


// This test verifies that a resource limitation incurred on a nested
// container is propagated all the way up to the scheduler.
TEST_P_TEMP_DISABLED_ON_WINDOWS(DefaultExecutorTest, ResourceLimitation)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();
  flags.enforce_container_disk_quota = true;
  flags.container_disk_watch_interval = Milliseconds(1);
  flags.isolation = "disk/du";

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
    v1::Resources::parse("cpus:0.1;mem:32;disk:10").get();

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

  Future<v1::scheduler::Event::Update> starting;
  Future<v1::scheduler::Event::Update> running;
  Future<v1::scheduler::Event::Update> failed;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&starting),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&running),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&failed),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Since we requested 10MB each for the task and the executor,
  // writing 30MB will violate our disk resource limit.
  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
      "dd if=/dev/zero of=dd.out bs=1048576 count=30; sleep 1000");

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(starting);

  EXPECT_EQ(v1::TASK_STARTING, starting->status().state());
  EXPECT_EQ(taskInfo.task_id(), starting->status().task_id());

  AWAIT_READY(running);

  EXPECT_EQ(v1::TASK_RUNNING, running->status().state());
  EXPECT_EQ(taskInfo.task_id(), running->status().task_id());

  AWAIT_READY(failed);

  // We expect the failure to be a disk limitation that tells us something
  // about the disk resources.
  EXPECT_EQ(v1::TASK_FAILED, failed->status().state());
  EXPECT_EQ(
      v1::TaskStatus::REASON_CONTAINER_LIMITATION_DISK,
      failed->status().reason());

  EXPECT_EQ(taskInfo.task_id(), failed->status().task_id());
  ASSERT_TRUE(failed->status().has_limitation());
  EXPECT_GT(failed->status().limitation().resources().size(), 0);

  foreach (const v1::Resource& resource,
           failed->status().limitation().resources()) {
    EXPECT_EQ("disk", resource.name());
    EXPECT_EQ(mesos::v1::Value::SCALAR, resource.type());
  }
}


struct LauncherAndIsolationParam
{
  LauncherAndIsolationParam(const string& _launcher, const string& _isolation)
    : launcher(_launcher), isolation(_isolation) {}

  const string launcher;
  const string isolation;
};


// This test verifies that URIs set on tasks are fetched and made available to
// them when started by the DefaultExecutor.
TEST_P(DefaultExecutorTest, TaskWithFileURI)
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

#ifndef __WINDOWS__
  const std::string contentTest = "test `cat testFile` = pizza";
#else
  const std::string contentTest = "";
#endif // __WINDOWS__

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      contentTest);

#ifdef __WINDOWS__
  taskInfo.mutable_command()->set_shell(false);
  taskInfo.mutable_command()->set_value("powershell.exe");
  taskInfo.mutable_command()->add_arguments("powershell.exe");
  taskInfo.mutable_command()->add_arguments("-NoProfile");
  taskInfo.mutable_command()->add_arguments("-Command");
  taskInfo.mutable_command()->add_arguments(
      "if ((Get-Content testFile) -NotMatch 'pizza') { exit 1 }");
#endif // __WINDOWS__

  taskInfo.mutable_command()->add_uris()->set_value(
      uri::from_path(testFilePath));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
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

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(v1::TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}


// This test verifies that URIs set on Docker tasks are fetched and made
// available to them when started by the DefaultExecutor.
//
// TODO(coffler): This test is dependent on alpine image. For Windows,
// we'll need to port to use a Windows container, using PowerShell
// snippet from test "TaskWithFileURI" rather than Linux "test" command.
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

  taskInfo.mutable_command()->add_uris()->set_value(
      uri::from_path(testFilePath));

  mesos::v1::Image image;
  image.set_type(mesos::v1::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  mesos::v1::ContainerInfo* container = taskInfo.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
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

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(v1::TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}


class PersistentVolumeDefaultExecutor
  : public MesosTest,
    public WithParamInterface<LauncherAndIsolationParam>
{
public:
  PersistentVolumeDefaultExecutor() : param(GetParam()) {}

protected:
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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
        frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
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

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(FutureArg<1>(&updateFinished));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFinished->status().task_id());

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      devolve(volume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}


// This test verifies that the default executor mounts the persistent volume
// in the task container when it is set on a task in the task group, and the
// task's volume directory can be accessed from the `/files` endpoint.
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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
      frameworkInfo.roles(0),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  // Launch a task that expects the persistent volume to be
  // mounted in its sandbox.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      reserved.apply(v1::CREATE(volume)).get(),
      "echo abc > task_volume_path/file && sleep 1000");

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      devolve(volume));

  string filePath = path::join(volumePath, "file");

  // Wait up to 10 seconds for the task to write a file into the volume.
  Duration waited = Duration::zero();
  do {
    if (os::exists(filePath)) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < Seconds(10));

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_TRUE(os::exists(filePath));
  EXPECT_SOME_EQ("abc\n", os::read(filePath));

  v1::ContainerStatus status = updateRunning->status().container_status();

  ASSERT_TRUE(status.has_container_id());
  EXPECT_TRUE(status.container_id().has_parent());

  v1::ContainerID executorContainerId = status.container_id().parent();

  string taskPath = slave::paths::getTaskPath(
      flags.work_dir,
      devolve(offer.agent_id()),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()),
      devolve(executorContainerId),
      devolve(taskInfo.task_id()));

  string taskVolumePath =
    path::join(taskPath, volume.disk().volume().container_path());

  // Ensure the task's volume directory can be accessed from
  // the `/files` endpoint.
  process::UPID files("files", slave.get()->pid.address);

  {
    string query = string("path=") + taskVolumePath;
    Future<Response> response = process::http::get(
        files,
        "browse",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Array> parse = JSON::parse<JSON::Array>(response->body);
    ASSERT_SOME(parse);
    EXPECT_NE(0u, parse->values.size());
  }

  {
    string query =
      string("path=") + path::join(taskVolumePath, "file") + "&offset=0";

    Future<Response> response = process::http::get(
        files,
        "read",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);

    JSON::Object expected;
    expected.values["offset"] = 0;
    expected.values["data"] = "abc\n";

    AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);
  }
}


#ifndef __WINDOWS__
// This test verifies that the default executor mounts the local
// persistent volume in the task container when it is set on a
// task launched with a non-root user in the task group, and the
// task can write to the local persistent volume.
TEST_P(
    PersistentVolumeDefaultExecutor,
    ROOT_UNPRIVILEGED_USER_TaskSandboxLocalPersistentVolume)
{
  if (!strings::contains(param.isolation, "filesystem/linux")) {
    // Only run this test when the `filesystem/linux` isolator is enabled
    // since that is the only case the `volume/sandbox_path` isolator will
    // call volume gid manager.
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = param.launcher;
  flags.isolation = param.isolation;
  flags.volume_gid_range = "[10000-20000]";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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

  // Create a local persistent volume, and then launch a task with
  // a non-root user in a task group to write a file to the volume.
  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  v1::CommandInfo command =
      v1::createCommandInfo("echo abc > task_volume_path/file");

  command.set_user(user.get());

  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      reserved.apply(v1::CREATE(volume)).get(),
      command);

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateFinished),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
}


// This test verifies that the default executor mounts the shared
// persistent volume in the task container when it is set on a task
// launched with a non-root user in the task group, and the task can
// write to the shared persistent volume.
TEST_P(
    PersistentVolumeDefaultExecutor,
    ROOT_UNPRIVILEGED_USER_TaskSandboxSharedPersistentVolume)
{
  if (!strings::contains(param.isolation, "filesystem/linux")) {
    // Only run this test when the `filesystem/linux` isolator is enabled
    // since that is the only case the `volume/sandbox_path` isolator will
    // call volume gid manager.
    return;
  }

  // Reinitialize libprocess to ensure volume gid manager's metrics
  // can be added in each iteration of this test (i.e., run this test
  // repeatedly with the `--gtest_repeat` option).
  process::reinitialize(None(), None(), None());

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = param.launcher;
  flags.isolation = param.isolation;
  flags.volume_gid_range = "[10000-20000]";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  // Set the framework user to a non-root user and the task will
  // be launched with this user by default.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo.set_user(user.get());
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::SHARED_RESOURCES);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  // Create a shared persistent volume, and then launch a
  // task in a task group to write a file to the volume.
  v1::Resource volume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal(),
      true);

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      reserved.apply(v1::CREATE(volume)).get(),
      "echo abc > task_volume_path/file");

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateFinished),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFinished->status().task_id());

  // One gid should have been allocated to the volume. Please note that shared
  // persistent volume's gid will be deallocated only when it is destroyed.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 1,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());
}
#endif // __WINDOWS__


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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
    v1::Resources::parse("cpus:0.1;mem:32;disk:32")
      ->pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::Resources totalResources =
    v1::Resources::parse("cpus:0.3;mem:96;disk:96")
      ->pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::Resource executorVolume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
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

  vector<Future<v1::scheduler::Event::Update>> updates(6);

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
    STARTING,
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
        ASSERT_EQ(v1::TASK_STARTING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::STARTING;

        break;
      }
      case Stage::STARTING: {
        ASSERT_EQ(v1::TASK_RUNNING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::RUNNING;

        break;
      }
      case Stage::RUNNING: {
        ASSERT_EQ(v1::TASK_FINISHED, taskStatus.state());

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
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
    v1::Resources::parse("cpus:0.1;mem:32;disk:32")
      ->pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::Resources totalResources =
    v1::Resources::parse("cpus:0.3;mem:96;disk:96")
      ->pushReservation(
          v1::createDynamicReservationInfo(
              frameworkInfo.roles(0), frameworkInfo.principal()));

  v1::Resource executorVolume = v1::createPersistentVolume(
      Megabytes(1),
      frameworkInfo.roles(0),
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

  vector<Future<v1::scheduler::Event::Update>> updates(6);

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
    STARTING,
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
        ASSERT_EQ(v1::TASK_STARTING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::STARTING;

        break;
      }
      case Stage::STARTING: {
        ASSERT_EQ(v1::TASK_RUNNING, taskStatus.state());

        taskStages[taskStatus.task_id()] = Stage::RUNNING;

        break;
      }
      case Stage::RUNNING: {
        ASSERT_EQ(v1::TASK_FINISHED, taskStatus.state());

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


// This test verifies that the command health checks initiated by the default
// executor are able to read files in a persistent volume.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    PersistentVolumeDefaultExecutor, ROOT_HealthCheckUsingPersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = param.launcher;
  flags.isolation = param.isolation;

  Fetcher fetcher(flags);

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command health
  // checks) tries to start a IO switchboard, which doesn't work in
  // local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

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
      frameworkInfo.roles(0),
      "id1",
      "task_volume_path",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  v1::Resources reserved =
    unreserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  // Launch a task that expects the persistent volume to be
  // mounted in its sandbox.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      reserved.apply(v1::CREATE(volume)).get(),
      "echo abc > task_volume_path/file && sleep 31337");

  // Create a health check that will only pass if it is able to read
  // from the task's persistent volume.
  v1::HealthCheck healthCheck;
  healthCheck.set_type(v1::HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("cat task_volume_path/file");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(10);

  taskInfo.mutable_health_check()->CopyFrom(healthCheck);

  v1::Offer::Operation reserve = v1::RESERVE(reserved);
  v1::Offer::Operation create = v1::CREATE(volume);
  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  Future<Event::Update> updateHealthy;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&updateStarting),
            v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())))
    .WillOnce(
        DoAll(
            FutureArg<1>(&updateRunning),
            v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())))
    .WillOnce(
        DoAll(
            FutureArg<1>(&updateHealthy),
            v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {reserve, create, launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateHealthy);
  EXPECT_EQ(v1::TASK_RUNNING, updateHealthy->status().state());
  EXPECT_EQ(
      v1::TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      updateHealthy->status().reason());
  EXPECT_TRUE(updateHealthy->status().has_healthy());
  EXPECT_TRUE(updateHealthy->status().healthy());

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      devolve(volume));

  string filePath = path::join(volumePath, "file");

  // Ensure that the task was able to write to the persistent volume.
  EXPECT_SOME_EQ("abc\n", os::read(filePath));
}


// This is a regression test for MESOS-8468. It verifies that upon a
// `LAUNCH_GROUP` failure the default executor kills the corresponding task
// group, but that it doesn't affect tasks from other task groups.
TEST_P_TEMP_DISABLED_ON_WINDOWS(DefaultExecutorTest, ROOT_LaunchGroupFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Configure the agent in such a way that tasks requiring an appc image will
  // pass validation (won't result in a TASK_ERROR), but will trigger a
  // `LAUNCH_NESTED_CONTAINER` failure.
  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();
  flags.isolation = "filesystem/linux";
  flags.image_providers = "APPC";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

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

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo sleepTaskInfo1 = v1::createTask(
      agentId,
      resources,
      SLEEP_COMMAND(1000),
      None(),
      "sleepTask1",
      "sleepTask1");

  v1::TaskGroupInfo taskGroup1 =
    v1::createTaskGroupInfo({sleepTaskInfo1});

  v1::TaskInfo sleepTaskInfo2 = v1::createTask(
      agentId,
      resources,
      SLEEP_COMMAND(1000),
      None(),
      "sleepTask2",
      "sleepTask2");

  // Create a task that requires an appc image. The agent wasn't configured to
  // support appc, so it should trigger a `LAUNCH_NESTED_CONTAINER` failure.
  v1::TaskInfo failingTaskInfo = v1::createTask(
      agentId,
      resources,
      SLEEP_COMMAND(1000),
      None(),
      "failingTask",
      "failingTask");

  mesos::v1::ContainerInfo* container = failingTaskInfo.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);

  mesos::v1::Image* image = container->mutable_mesos()->mutable_image();
  image->set_type(mesos::v1::Image::APPC);
  image->mutable_appc()->set_name("foobar");

  v1::TaskGroupInfo taskGroup2 =
    v1::createTaskGroupInfo({sleepTaskInfo2, failingTaskInfo});

  testing::Sequence sleepTask1;
  Future<v1::scheduler::Event::Update> sleepTaskStartingUpdate1;
  Future<v1::scheduler::Event::Update> sleepTaskRunningUpdate1;
  Future<v1::scheduler::Event::Update> sleepTaskKilledUpdate1;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(sleepTask1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskStartingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(sleepTask1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskRunningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(sleepTask1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskKilledUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  testing::Sequence sleepTask2;
  Future<v1::scheduler::Event::Update> sleepTaskStartingUpdate2;
  Future<v1::scheduler::Event::Update> sleepTaskRunningUpdate2;
  Future<v1::scheduler::Event::Update> sleepTaskKilledUpdate2;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(sleepTask2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskStartingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(sleepTask2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskRunningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(sleepTaskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(sleepTask2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&sleepTaskKilledUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  testing::Sequence failingTask;
  Future<v1::scheduler::Event::Update> failingTaskStartingUpdate;
  Future<v1::scheduler::Event::Update> failingTaskFailedUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(failingTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(failingTask)
    .WillOnce(
        DoAll(
            FutureArg<1>(&failingTaskStartingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(failingTaskInfo.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FAILED))))
    .InSequence(failingTask)
    .WillOnce(
        DoAll(
            FutureArg<1>(&failingTaskFailedUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::LAUNCH_GROUP(executorInfo, taskGroup1),
       v1::LAUNCH_GROUP(executorInfo, taskGroup2)}));

  // The `LAUNCH_NESTED_CONTAINER` call for `failingTask` should fail, bacause
  // it requires an appc image, but the agent is not properly configured to
  // support fetching appc images.
  //
  // That means that the default executor should send `TASK_STARTING` followed
  // by `TASK_FAILED`.
  AWAIT_READY(failingTaskStartingUpdate);
  AWAIT_READY(failingTaskFailedUpdate);

  // The default executor will be able to launch `sleepTask2`, so it should
  // send `TASK_STARTING` and `TASK_RUNNING` updates. The task is in the same
  // task group as `failingTask`, so the executor should kill it when
  // `failingTask` fails to launch.
  AWAIT_READY(sleepTaskStartingUpdate2);
  AWAIT_READY(sleepTaskRunningUpdate2);
  AWAIT_READY(sleepTaskKilledUpdate2);

  // The default executor will be able to launch `sleepTask1`, so it should
  // send `TASK_STARTING` and `TASK_RUNNING` updates. The task is NOT in the
  // same task group as `failingTask`, so it shouldn't be killed until the
  // scheduler sends a kill request.
  AWAIT_READY(sleepTaskStartingUpdate1);
  AWAIT_READY(sleepTaskRunningUpdate1);
  ASSERT_TRUE(sleepTaskKilledUpdate1.isPending());

  // The executor should still be alive after the second task group has been
  // killed.
  ASSERT_TRUE(executorFailure.isPending());

  // Now kill the only task present in the first task group.
  mesos.send(v1::createCallKill(frameworkId, sleepTaskInfo1.task_id()));

  AWAIT_READY(sleepTaskKilledUpdate1);

  // The executor should commit suicide after all the tasks have been
  // killed.
  AWAIT_READY(executorFailure);
}


// This test verifies that the `MESOS_ALLOCATION_ROLE`
// environment variable is set properly.
TEST_P(DefaultExecutorTest, AllocationRoleEnvironmentVariable)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Start the framework with a role specified.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

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

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
        DoAll(
            FutureArg<1>(&finishedUpdate),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
#ifdef __WINDOWS__
      "if %MESOS_ALLOCATION_ROLE% == \"role1\" (exit 1)");
#else
      "if [ \"$MESOS_ALLOCATION_ROLE\" != \"role1\" ]; then exit 1; fi");
#endif // __WINDOWS__

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(v1::TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
