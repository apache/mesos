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
#include <string>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>

#include <mesos/v1/master/master.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "master/registry_operations.hpp"

#include "messages/messages.hpp"

#include "tests/cluster.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

namespace http = process::http;


using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;

using std::vector;

using testing::_;
using testing::AllOf;
using testing::DoAll;
using testing::Not;
using testing::Return;
using testing::Sequence;
using testing::Truly;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class MasterAlreadyDrainedTest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  // Creates a master and agent.
  void SetUp() override
  {
    MesosTest::SetUp();

    Clock::pause();

    // Create the master.
    masterFlags = CreateMasterFlags();
    Try<Owned<cluster::Master>> _master = StartMaster(masterFlags);
    ASSERT_SOME(_master);
    master = _master.get();

    Future<SlaveRegisteredMessage> slaveRegisteredMessage =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

    // Create the agent.
    agentFlags = CreateSlaveFlags();
    detector = master.get()->createDetector();
    Try<Owned<cluster::Slave>> _slave = StartSlave(detector.get(), agentFlags);
    ASSERT_SOME(_slave);
    slave = _slave.get();

    Clock::advance(agentFlags.registration_backoff_factor);
    AWAIT_READY(slaveRegisteredMessage);
    agentId = evolve(slaveRegisteredMessage->slave_id());
  }

  void TearDown() override
  {
    slave.reset();
    detector.reset();
    master.reset();

    Clock::resume();

    MesosTest::TearDown();
  }

  master::Flags CreateMasterFlags() override
  {
    // Turn off periodic allocations to avoid the race between
    // `HierarchicalAllocator::updateAvailable()` and periodic allocations.
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Seconds(1000);
    return flags;
  }

  // Helper function to post a request to "/api/v1" master endpoint and return
  // the response.
  static Future<http::Response> post(
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType,
      const Credential& credential = DEFAULT_CREDENTIAL)
  {
    http::Headers headers = createBasicAuthHeaders(credential);
    headers["Accept"] = stringify(contentType);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  }

protected:
  master::Flags masterFlags;
  Owned<cluster::Master> master;
  Owned<MasterDetector> detector;

  slave::Flags agentFlags;
  Owned<cluster::Slave> slave;
  v1::AgentID agentId;
};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    MasterAlreadyDrainedTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// When an operator submits a DRAIN_AGENT call, the agent with nothing running
// should be immediately transitioned to the DRAINED state.
TEST_P(MasterAlreadyDrainedTest, DrainAgent)
{
  Future<Nothing> registrarApplyDrained;
  EXPECT_CALL(*master->registrar, apply(_))
    .WillOnce(DoDefault())
    .WillOnce(DoAll(
        FutureSatisfy(&registrarApplyDrained),
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply)));

  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);
    drainAgent.mutable_max_grace_period()->set_seconds(10);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(registrarApplyDrained);

  mesos::v1::DrainInfo drainInfo;
  drainInfo.set_state(mesos::v1::DRAINED);
  drainInfo.mutable_config()->set_mark_gone(false);
  drainInfo.mutable_config()->mutable_max_grace_period()
    ->set_nanoseconds(Seconds(10).ns());

  // Ensure that the agent's drain info is reflected in the master's
  // GET_AGENTS response.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.deactivated(), true);

    EXPECT_EQ(agent.drain_info(), drainInfo);
    EXPECT_LT(0, agent.estimated_drain_start_time().nanoseconds());
  }
}


// This is a regression test for MESOS-10116.
// It verifies that reactivating an agent while it is not connected
// does not trigger generating offers for this agent, but instead makes
// the agent offered when it re-registers.
//
// Also, the test ensures that accepting the first offer for a reconnected agent
// does not crash the master and that this offer can be successfully used
// to launch a task. The latter serves as a regression test for MESOS-10118.
TEST_P(MasterAlreadyDrainedTest, ReactivateDisconnectedAgent)
{
  const ContentType contentType = GetParam();

  const auto IsMarkAgentDrained =
    [](const process::Owned<master::RegistryOperation>& operation) {
      return dynamic_cast<master::MarkAgentDrained*>(operation.get()) !=
             nullptr;
    };

  Future<Nothing> registrarApplyDrained;
  EXPECT_CALL(*master->registrar, apply(Truly(IsMarkAgentDrained)))
    .WillOnce(DoAll(
        FutureSatisfy(&registrarApplyDrained),
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply)));

  EXPECT_CALL(*master->registrar, apply(Not(Truly(IsMarkAgentDrained))))
    .WillRepeatedly(
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply));


  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);
    drainAgent.mutable_max_grace_period()->set_seconds(10);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(registrarApplyDrained);

  // The agent should apply draining as well.
  Clock::settle();

  // Simulate agent crash.
  slave->terminate();
  slave.reset();

  Clock::settle();

  // Set up the scheduler.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::PARTITION_AWARE);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Expect no offers after agent reactivation.
  EXPECT_CALL(*scheduler, offers(_, _))
    .Times(testing::AtMost(0));

  // Subscribe the scheduler.
  v1::scheduler::TestMesos mesos(master.get()->pid, contentType, scheduler);

  AWAIT_READY(subscribed);
  const v1::FrameworkID frameworkId = subscribed->framework_id();

  // Later, after agent reconnection, the scheduler will launch a task.
  // It should expect and acknowledge all task status updates.
  // Note that we will be specifically waiting for TASK_RUNNING update.
  const auto sendAcknowledge =
    v1::scheduler::SendAcknowledge(frameworkId, agentId);

  EXPECT_CALL(
      *scheduler, update(_, Not(TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .WillRepeatedly(sendAcknowledge);

  Future<v1::scheduler::Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .WillOnce(DoAll(FutureArg<1>(&taskRunning), sendAcknowledge));

  // Reactivate the agent.
  {
    v1::master::Call::ReactivateAgent reactivateAgent;
    reactivateAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::REACTIVATE_AGENT);
    call.mutable_reactivate_agent()->CopyFrom(reactivateAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  // Trigger allocation to make sure that the agent is not offered.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Expect to get an offer after the agent is brought back.
  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  Try<Owned<cluster::Slave>> recoveredSlave =
    StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(recoveredSlave);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  EXPECT_EQ(agentId, offer.agent_id());

  // Launch a task to verify that the offer is usable.
  const v1::TaskInfo taskInfo =
    v1::createTask(agentId, offer.resources(), SLEEP_COMMAND(1000));

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(taskRunning);
}


// When an operator submits a DRAIN_AGENT call with 'mark_gone == true',
// and the agent is not running anything, the agent should immediately be
// marked gone.
TEST_P(MasterAlreadyDrainedTest, DrainAgentMarkGone)
{
  // When the terminal ACK is received by the master, the agent should be marked
  // gone, which entails sending a `ShutdownMessage`.
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), _, _);

  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);
    drainAgent.set_mark_gone(true);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(shutdownMessage);
}


// When an operator submits a DRAIN_AGENT call with an agent that has
// momentarily disconnected, the call should succeed, and the agent should
// be drained when it returns to the cluster.
TEST_P(MasterAlreadyDrainedTest, DrainAgentDisconnected)
{
  // Simulate an agent crash, so that it disconnects from the master.
  slave->terminate();
  slave.reset();

  ContentType contentType = GetParam();

  // Ensure that the agent is disconnected (not active).
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.active(), false);
    EXPECT_EQ(agent.deactivated(), false);
  }

  // Start draining the disconnected agent.
  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  // Bring the agent back.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<DrainSlaveMessage> drainSlaveMesage =
    FUTURE_PROTOBUF(DrainSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> recoveredSlave =
    StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(recoveredSlave);

  Clock::advance(agentFlags.executor_reregistration_timeout);
  Clock::settle();
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(slaveReregisteredMessage);

  // The agent should be told to drain once it reregisters.
  AWAIT_READY(drainSlaveMesage);

  // Ensure that the agent is marked as DRAINED in the master now.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.deactivated(), true);
    EXPECT_EQ(mesos::v1::DRAINED, agent.drain_info().state());
  }
}


// When an operator submits a DRAIN_AGENT call for an agent that has gone
// unreachable, the call should succeed, and the agent should be drained
// if/when it returns to the cluster.
TEST_P(MasterAlreadyDrainedTest, DrainAgentUnreachable)
{
  Future<Owned<master::RegistryOperation>> registrarApplyUnreachable;
  EXPECT_CALL(*master->registrar, apply(_))
    .WillOnce(DoAll(
        FutureArg<0>(&registrarApplyUnreachable),
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply)))
    .WillRepeatedly(DoDefault());

  // Simulate an agent crash, so that it disconnects from the master.
  slave->terminate();
  slave.reset();

  Clock::advance(masterFlags.agent_reregister_timeout);
  AWAIT_READY(registrarApplyUnreachable);
  ASSERT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveUnreachable*>(
          registrarApplyUnreachable->get()));

  // Start draining the unreachable agent.
  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  // Bring the agent back.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<DrainSlaveMessage> drainSlaveMesage =
    FUTURE_PROTOBUF(DrainSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> recoveredSlave =
    StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(recoveredSlave);

  Clock::advance(agentFlags.executor_reregistration_timeout);
  Clock::settle();
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(slaveReregisteredMessage);

  // The agent should be told to drain once it reregisters.
  AWAIT_READY(drainSlaveMesage);

  // Ensure that the agent is marked as DRAINED in the master now.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.deactivated(), true);
    EXPECT_EQ(mesos::v1::DRAINED, agent.drain_info().state());
  }
}


class MasterDrainingTest
  : public MasterAlreadyDrainedTest
{
public:
  // Creates a master, agent, framework, and launches one sleep task.
  void SetUp() override
  {
    MasterAlreadyDrainedTest::SetUp();

    // Create the framework.
    scheduler = std::make_shared<v1::MockHTTPScheduler>();

    frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_checkpoint(true);
    frameworkInfo.add_capabilities()->set_type(
        v1::FrameworkInfo::Capability::PARTITION_AWARE);

    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

    Future<v1::scheduler::Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    Future<v1::scheduler::Event::Offers> offers;
    EXPECT_CALL(*scheduler, offers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return());

    mesos = std::make_shared<v1::scheduler::TestMesos>(
        master.get()->pid, ContentType::PROTOBUF, scheduler);

    AWAIT_READY(subscribed);
    frameworkId = subscribed->framework_id();

    // Launch a sleep task.
    AWAIT_READY(offers);
    ASSERT_FALSE(offers->offers().empty());

    const v1::Offer& offer = offers->offers(0);

    Try<v1::Resources> resources =
      v1::Resources::parse("cpus:0.1;mem:64;disk:64");

    ASSERT_SOME(resources);

    taskInfo = v1::createTask(agentId, resources.get(), SLEEP_COMMAND(1000));

    testing::Sequence updateSequence;
    Future<v1::scheduler::Event::Update> startingUpdate;
    Future<v1::scheduler::Event::Update> runningUpdate;

    // Make sure the agent receives these two acknowledgements.
    Future<StatusUpdateAcknowledgementMessage> startingAck =
      FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);
    Future<StatusUpdateAcknowledgementMessage> runningAck =
      FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

    EXPECT_CALL(
        *scheduler,
        update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_STARTING))))
      .InSequence(updateSequence)
      .WillOnce(DoAll(
          FutureArg<1>(&startingUpdate),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)));

    EXPECT_CALL(
        *scheduler,
        update(_, AllOf(
              TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
              TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
      .InSequence(updateSequence)
      .WillOnce(DoAll(
          FutureArg<1>(&runningUpdate),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)));

    mesos->send(
        v1::createCallAccept(
            frameworkId,
            offer,
            {v1::LAUNCH({taskInfo})}));

    AWAIT_READY(startingUpdate);
    AWAIT_READY(startingAck);
    AWAIT_READY(runningUpdate);
    AWAIT_READY(runningAck);
  }

  void TearDown() override
  {
    mesos.reset();
    scheduler.reset();

    MasterAlreadyDrainedTest::TearDown();
  }

protected:
  std::shared_ptr<v1::MockHTTPScheduler> scheduler;
  v1::FrameworkInfo frameworkInfo;
  std::shared_ptr<v1::scheduler::TestMesos> mesos;
  v1::FrameworkID frameworkId;

  v1::TaskInfo taskInfo;
};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    MasterDrainingTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// When an operator submits a DRAIN_AGENT call, the agent should kill all
// running tasks.
TEST_P(MasterDrainingTest, DrainAgent)
{
  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .WillOnce(DoAll(
        FutureArg<1>(&killedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<StatusUpdateAcknowledgementMessage> killedAck =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  Future<Nothing> registrarApplyDrained;
  Future<Nothing> registrarApplyReactivated;
  EXPECT_CALL(*master->registrar, apply(_))
    .WillOnce(DoDefault())
    .WillOnce(DoAll(
        FutureSatisfy(&registrarApplyDrained),
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply)))
    .WillOnce(DoAll(
        FutureSatisfy(&registrarApplyReactivated),
        Invoke(master->registrar.get(), &MockRegistrar::unmocked_apply)));

  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);
    drainAgent.mutable_max_grace_period()->set_seconds(0);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(killedUpdate);
  AWAIT_READY(killedAck);
  AWAIT_READY(registrarApplyDrained);

  // Ensure that the update acknowledgement has been processed.
  Clock::settle();

  mesos::v1::DrainInfo drainInfo;
  drainInfo.set_state(mesos::v1::DRAINED);
  drainInfo.mutable_config()->set_mark_gone(false);
  drainInfo.mutable_config()->mutable_max_grace_period()->set_nanoseconds(0);

  // Ensure that the agent's drain info is reflected in the master's
  // GET_AGENTS response.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.deactivated(), true);

    EXPECT_EQ(agent.drain_info(), drainInfo);
    EXPECT_LT(0, agent.estimated_drain_start_time().nanoseconds());
  }

  // Ensure that the agent's drain info is reflected in the master's
  // '/state' response.
  {
    Future<process::http::Response> response = process::http::get(
        master->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Object> stateDrainInfo = parse->find<JSON::Object>(
        "slaves[0].drain_info");

    ASSERT_SOME_EQ(JSON::protobuf(drainInfo), stateDrainInfo);

    Result<JSON::Number> stateDrainStartTime = parse->find<JSON::Number>(
        "slaves[0].estimated_drain_start_time_seconds");

    ASSERT_SOME(stateDrainStartTime);
    EXPECT_LT(0, stateDrainStartTime->as<int>());
  }

  // Ensure that the agent's drain info is reflected in the master's
  // '/state-summary' response.
  {
    Future<process::http::Response> response = process::http::get(
        master->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Object> stateDrainInfo = parse->find<JSON::Object>(
        "slaves[0].drain_info");

    ASSERT_SOME_EQ(JSON::protobuf(drainInfo), stateDrainInfo);

    Result<JSON::Number> stateDrainStartTime =
      parse->find<JSON::Number>("slaves[0].estimated_drain_start_time_seconds");

    ASSERT_SOME(stateDrainStartTime);
    EXPECT_LT(0, stateDrainStartTime->as<int>());
  }

  // Reactivate the agent and expect to get the agent in an offer.
  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    v1::master::Call::ReactivateAgent reactivateAgent;
    reactivateAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::REACTIVATE_AGENT);
    call.mutable_reactivate_agent()->CopyFrom(reactivateAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(registrarApplyReactivated);

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());
  EXPECT_EQ(agentId, offers->offers(0).agent_id());
}


// When an operator submits a DRAIN_AGENT call with 'mark_gone == true', the
// agent should kill all running tasks and the master should mark the agent gone
// once terminal ACKs have been received.
TEST_P(MasterDrainingTest, DrainAgentMarkGone)
{
  Future<v1::scheduler::Event::Update> goneUpdate;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_GONE_BY_OPERATOR))))
    .WillOnce(DoAll(
        FutureArg<1>(&goneUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // When the terminal ACK is received by the master, the agent should be marked
  // gone, which entails sending a `ShutdownMessage`.
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), _, _);

  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);
    drainAgent.set_mark_gone(true);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  AWAIT_READY(goneUpdate);
  AWAIT_READY(shutdownMessage);
}


// When an operator submits a DRAIN_AGENT call with an agent that has
// momentarily disconnected, the call should succeed, and the agent should
// be drained when it returns to the cluster.
TEST_P(MasterDrainingTest, DrainAgentDisconnected)
{
  // Simulate an agent crash, so that it disconnects from the master.
  slave->terminate();
  slave.reset();

  ContentType contentType = GetParam();

  // Ensure that the agent is disconnected (not active).
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.active(), false);
    EXPECT_EQ(agent.deactivated(), false);
  }

  // Start draining the disconnected agent.
  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  // Bring the agent back.
  Future<ReregisterExecutorMessage> reregisterExecutor =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<DrainSlaveMessage> drainSlaveMesage =
    FUTURE_PROTOBUF(DrainSlaveMessage(), _, _);

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .WillOnce(DoAll(
        FutureArg<1>(&killedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<StatusUpdateAcknowledgementMessage> killedAck =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  Try<Owned<cluster::Slave>> recoveredSlave =
    StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(recoveredSlave);

  AWAIT_READY(reregisterExecutor);
  Clock::advance(agentFlags.executor_reregistration_timeout);
  Clock::settle();
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(slaveReregisteredMessage);

  // The agent should be told to drain once it reregisters.
  AWAIT_READY(drainSlaveMesage);
  AWAIT_READY(killedUpdate);
  AWAIT_READY(killedAck);

  // Ensure that the agent is marked as DRAINED in the master now.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_AGENTS);

    Future<http::Response> response =
      post(master->pid, call, contentType);
    AWAIT_ASSERT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> getAgents =
      deserialize<v1::master::Response>(contentType, response->body);
    ASSERT_SOME(getAgents);

    ASSERT_EQ(v1::master::Response::GET_AGENTS, getAgents->type());
    ASSERT_EQ(getAgents->get_agents().agents_size(), 1);

    const v1::master::Response::GetAgents::Agent& agent =
        getAgents->get_agents().agents(0);

    EXPECT_EQ(agent.deactivated(), true);
    EXPECT_EQ(mesos::v1::DRAINED, agent.drain_info().state());
  }

  // Reactivate the agent and expect to get the agent in an offer.
  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    v1::master::Call::ReactivateAgent reactivateAgent;
    reactivateAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::REACTIVATE_AGENT);
    call.mutable_reactivate_agent()->CopyFrom(reactivateAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());
  EXPECT_EQ(agentId, offers->offers(0).agent_id());
}


// When an operator submits a DRAIN_AGENT call with an agent that has gone
// unreachable, the call should succeed, and the agent should be drained
// if/when it returns to the cluster.
TEST_P(MasterDrainingTest, DrainAgentUnreachable)
{
  testing::Sequence updateSequence;
  Future<v1::scheduler::Event::Update> unreachableUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> killedUpdate;

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_UNREACHABLE))))
    .InSequence(updateSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&unreachableUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // When the agent is brought back, we expect a TASK_RUNNING followed by
  // a TASK_KILLED (due to draining).
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(updateSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(updateSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&killedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<StatusUpdateAcknowledgementMessage> killedAck =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  // Simulate an agent crash, so that it disconnects from the master.
  slave->terminate();
  slave.reset();

  Clock::advance(masterFlags.agent_reregister_timeout);
  AWAIT_READY(unreachableUpdate);

  // Start draining the unreachable agent.
  ContentType contentType = GetParam();

  {
    v1::master::Call::DrainAgent drainAgent;
    drainAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    call.mutable_drain_agent()->CopyFrom(drainAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  // Bring the agent back.
  Future<ReregisterExecutorMessage> reregisterExecutor =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<DrainSlaveMessage> drainSlaveMesage =
    FUTURE_PROTOBUF(DrainSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> recoveredSlave =
    StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(recoveredSlave);

  AWAIT_READY(reregisterExecutor);
  Clock::advance(agentFlags.executor_reregistration_timeout);
  Clock::settle();
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(slaveReregisteredMessage);

  // The agent should be told to drain once it reregisters.
  AWAIT_READY(drainSlaveMesage);
  AWAIT_READY(runningUpdate);
  AWAIT_READY(killedUpdate);
  AWAIT_READY(killedAck);

  // Reactivate the agent and expect to get the agent in an offer.
  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    v1::master::Call::ReactivateAgent reactivateAgent;
    reactivateAgent.mutable_agent_id()->CopyFrom(agentId);

    v1::master::Call call;
    call.set_type(v1::master::Call::REACTIVATE_AGENT);
    call.mutable_reactivate_agent()->CopyFrom(reactivateAgent);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        post(master->pid, call, contentType));
  }

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());
  EXPECT_EQ(agentId, offers->offers(0).agent_id());
}


class MasterDrainingTest2
  : public MesosTest,
    public WithParamInterface<ContentType> {};

// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    MasterDrainingTest2,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// This test ensures that the user cannot reactivate an agent
// that is still in the DRAINING state (which was previously
// possible and problematic, see MESOS-10096).
//
// We use a mock executor that ignores the kill task request
// to ensure the agent doesn't transition out of draining.
TEST_P(MasterDrainingTest2, DisallowReactivationWhileDraining)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  auto slaveOptions = SlaveOptions(detector.get())
                        .withFlags(CreateSlaveFlags())
                        .withId(process::ID::generate())
                        .withContainerizer(&containerizer);
  Try<Owned<cluster::Slave>> slave = StartSlave(slaveOptions);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Start the scheduler and launch a task.
  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  *task.mutable_slave_id() = offer.slave_id();
  *task.mutable_resources() = offer.resources();
  *task.mutable_executor() = DEFAULT_EXECUTOR_INFO;

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(containerizer, update(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Promise<Option<ContainerTermination>> hang;

  EXPECT_CALL(containerizer, wait(_))
    .WillRepeatedly(Return(hang.future()));

  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning1));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning1);
  ASSERT_EQ(TASK_RUNNING, statusRunning1->state());

  ContentType contentType = GetParam();

  // Start draining the agent.
  // Don't do anything upon the kill task request.
  EXPECT_CALL(exec, killTask(_, _));

  {
    v1::master::Call::DrainAgent drainAgent;
    *drainAgent.mutable_agent_id() = evolve(offer.slave_id());

    v1::master::Call call;
    call.set_type(v1::master::Call::DRAIN_AGENT);
    *call.mutable_drain_agent() = drainAgent;

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::OK().status,
        MasterAlreadyDrainedTest::post(
            (*master)->pid, call, contentType));
  }

  // The agent should now be in the draining state.

  // Attempt to reactivate the agent while it is in
  // draining, this should be rejected.
  {
    v1::master::Call::ReactivateAgent reactivateAgent;
    *reactivateAgent.mutable_agent_id() = evolve(offer.slave_id());

    v1::master::Call call;
    call.set_type(v1::master::Call::REACTIVATE_AGENT);
    call.mutable_reactivate_agent()->CopyFrom(reactivateAgent);

    Future<http::Response> response =
      MasterAlreadyDrainedTest::post((*master)->pid, call, contentType);

    AWAIT_READY(response);
    EXPECT_EQ(http::BadRequest().status, response->status);
    EXPECT_EQ("Agent is still in the DRAINING state",
              response->body);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
