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

#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/scheduler.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;

using process::http::OK;
using process::http::Response;

using std::vector;

using testing::_;
using testing::AtMost;

namespace mesos {
namespace internal {
namespace tests {

// This file contains upgrade integration tests. Note that tests
// in this file can only "spoof" an old version of a component,
// since these run against the current version of the repository.
// The long term plan for integration tests is to checkout
// different releases of the repository and run components from
// different releases against each other.

class UpgradeTest : public MesosTest {};


// This tests that when a non-MULTI_ROLE agent with task running
// re-registers with MULTI_ROLE master, master will inject the
// allocation role in the tasks and executors.
//
// Firstly we start a regular MULTI_ROLE agent and launch a long
// running task. Then we simulate a master detect event to trigger
// agent re-registration. We strip MULTI_ROLE capability and
// task/executor allocation_info from the agent's re-registration
// in order to spoof an old agent. We then inspect the `/state`
// endpoint to see `role` being set for task/executor.
TEST_F(UpgradeTest, ReregisterOldAgentWithMultiRoleMaster)
{
  Clock::pause();

  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Create a StandaloneMasterDetector to enable the agent to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get()->pid);

  // Start a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("foo");

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(2);
  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorInfo executorInfo =
    createExecutorInfo("default", "exit 1", "cpus:0.1");
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executorInfo, 1, 1, 64, "foo"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Cause the scheduler to re-register with the master.
  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a new master detected event on the agent,
  // so that the agent will do a re-registration.
  detector.appoint(master.get()->pid);

  Clock::settle();
  AWAIT_READY(disconnected);

  // Drop subsequent `ReregisterSlaveMessage`s to prevent a race condition
  // where an unspoofed retry reaches master before the spoofed one.
  DROP_PROTOBUFS(ReregisterSlaveMessage(), slave.get()->pid, master.get()->pid);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    DROP_PROTOBUF(
        ReregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(
        SlaveReregisteredMessage(),
        master.get()->pid,
        slave.get()->pid);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(reregisterSlaveMessage);

  EXPECT_EQ(1u, reregisterSlaveMessage->agent_capabilities_size());
  EXPECT_EQ(
      SlaveInfo::Capability::MULTI_ROLE,
      reregisterSlaveMessage->agent_capabilities(0).type());

  // Strip allocation_info and MULTI_ROLE capability.
  ReregisterSlaveMessage strippedReregisterSlaveMessage =
    reregisterSlaveMessage.get();
  strippedReregisterSlaveMessage.clear_agent_capabilities();

  foreach (ExecutorInfo& executorInfo,
           *strippedReregisterSlaveMessage.mutable_executor_infos()) {
    foreach (Resource& resource, *executorInfo.mutable_resources()) {
      resource.clear_allocation_info();
    }
  }

  foreach (Task& task, *strippedReregisterSlaveMessage.mutable_tasks()) {
    foreach (Resource& resource, *task.mutable_resources()) {
      resource.clear_allocation_info();
    }
  }

  // Prevent this from being dropped per the DROP_PROTOBUFS above.
  FUTURE_PROTOBUF(
      ReregisterSlaveMessage(),
      slave.get()->pid,
      master.get()->pid);

  process::post(
      slave.get()->pid,
      master.get()->pid,
      strippedReregisterSlaveMessage);

  Clock::settle();

  AWAIT_READY(slaveReregisteredMessage);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  EXPECT_EQ(1u, parse->values.count("frameworks"));
  JSON::Array frameworks = parse->at<JSON::Array>("frameworks").get();

  EXPECT_EQ(1u, frameworks.values.size());
  ASSERT_TRUE(frameworks.values.front().is<JSON::Object>());
  JSON::Object framework = frameworks.values.front().as<JSON::Object>();

  EXPECT_EQ(1u, framework.values.count("tasks"));
  JSON::Array tasks = framework.at<JSON::Array>("tasks").get();
  JSON::Object task = tasks.values.front().as<JSON::Object>();

  EXPECT_EQ(frameworkInfo.role(), task.values["role"]);

  EXPECT_EQ(1u, framework.values.count("executors"));
  JSON::Array executors = framework.at<JSON::Array>("executors").get();
  JSON::Object executor = executors.values.front().as<JSON::Object>();

  EXPECT_EQ(frameworkInfo.role(), executor.values["role"]);

  driver.stop();
  driver.join();
}


// Checks that resources of an agent will be offered to MULTI_ROLE
// frameworks after being upgraded to support MULTI_ROLE. We first
// strip MULTI_ROLE capability in `registerSlaveMessage` to spoof an
// old agent. Then we simulate a master detection event to trigger
// agent re-registration. After 'upgrade', resources of the agent
// should be offered to MULTI_ROLE frameworks.
TEST_F(UpgradeTest, UpgradeSlaveIntoMultiRole)
{
  Clock::pause();

  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Create a StandaloneMasterDetector to enable the agent to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get()->pid);

  // Start a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Drop subsequent `ReregisterSlaveMessage`s to prevent a race condition
  // where an unspoofed retry reaches master before the spoofed one.
  DROP_PROTOBUFS(RegisterSlaveMessage(), slave.get()->pid, master.get()->pid);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    DROP_PROTOBUF(
        RegisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(
        SlaveRegisteredMessage(),
        master.get()->pid,
        slave.get()->pid);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(registerSlaveMessage);

  EXPECT_EQ(1u, registerSlaveMessage->agent_capabilities_size());
  EXPECT_EQ(
      SlaveInfo::Capability::MULTI_ROLE,
      registerSlaveMessage->agent_capabilities(0).type());

  // Strip MULTI_ROLE capability.
  RegisterSlaveMessage strippedRegisterSlaveMessage =
    registerSlaveMessage.get();
  strippedRegisterSlaveMessage.clear_agent_capabilities();

  // Prevent this from being dropped per the DROP_PROTOBUFS above.
  FUTURE_PROTOBUF(
      RegisterSlaveMessage(),
      slave.get()->pid,
      master.get()->pid);

  process::post(
      slave.get()->pid,
      master.get()->pid,
      strippedRegisterSlaveMessage);

  Clock::settle();

  AWAIT_READY(slaveRegisteredMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_role();
  frameworkInfo.add_roles("foo");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  Clock::settle();

  AWAIT_READY(registered);

  // We don't expect scheduler to receive any offer because agent is not
  // MULTI_ROLE capable.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(0);

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // Simulate a new master detected event on the agent,
  // so that the agent will do a re-registration.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  detector.appoint(master.get()->pid);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveReregisteredMessage);

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // We should be able to receive offers after
  // agent capabilities being udpated.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
