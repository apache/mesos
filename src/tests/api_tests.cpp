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
#include <tuple>
#include <utility>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/resource_provider.hpp>

#include <mesos/v1/master/master.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/recordio.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/detector/standalone.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

#include "tests/containerizer/mock_containerizer.hpp"
#include "tests/master/mock_master_api_subscriber.hpp"

namespace http = process::http;

using std::shared_ptr;

using google::protobuf::RepeatedPtrField;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::slave::ContainerTermination;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;
using mesos::v1::typeutils::diff;

using mesos::internal::devolve;
using mesos::internal::evolve;

using mesos::internal::master::DEFAULT_HEARTBEAT_INTERVAL;

using mesos::internal::recordio::Reader;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;

using recordio::Decoder;

using std::set;
using std::string;
using std::tuple;
using std::vector;

using testing::_;
using testing::AllOf;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::Sequence;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class MasterAPITest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
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
  Future<v1::master::Response> post(
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
        stringify(contentType))
      .then([contentType](const http::Response& response)
            -> Future<v1::master::Response> {
        if (response.status != http::OK().status) {
          return Failure("Unexpected response status " + response.status);
        }
        return deserialize<v1::master::Response>(contentType, response.body);
      });
  }
};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    MasterAPITest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


TEST_P(MasterAPITest, GetAgents)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start one agent.
  Future<UpdateSlaveMessage> updateAgentMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.hostname = "host";
  slaveFlags.domain = createDomainInfo("region-xyz", "zone-456");

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateAgentMessage);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_AGENTS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
  ASSERT_EQ(v1Response->get_agents().agents_size(), 1);

  const v1::master::Response::GetAgents::Agent& v1Agent =
      v1Response->get_agents().agents(0);

  EXPECT_EQ("host", v1Agent.agent_info().hostname());
  EXPECT_EQ(evolve(slaveFlags.domain.get()), v1Agent.agent_info().domain());
  EXPECT_EQ(agent.get()->pid, v1Agent.pid());
  EXPECT_TRUE(v1Agent.active());
  EXPECT_EQ(MESOS_VERSION, v1Agent.version());
  EXPECT_EQ(4, v1Agent.total_resources_size());
  EXPECT_TRUE(v1Agent.resource_providers().empty());

  // Start a resource provider.
  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  v1::Resource resource = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  v1::TestResourceProvider resourceProvider(info, resource);

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  updateAgentMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateAgentMessage);

  v1Response = post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
  ASSERT_EQ(v1Response->get_agents().agents_size(), 1);
  ASSERT_FALSE(v1Response->get_agents().agents(0).resource_providers().empty());

  const mesos::v1::ResourceProviderInfo& responseInfo =
    v1Response->get_agents()
      .agents(0)
      .resource_providers(0)
      .resource_provider_info();

  EXPECT_EQ(info.type(), responseInfo.type());
  EXPECT_EQ(info.name(), responseInfo.name());

  ASSERT_TRUE(responseInfo.has_id());
  resource.mutable_provider_id()->CopyFrom(responseInfo.id());

  const v1::Resources responseResources =
    v1Response->get_agents()
      .agents(0)
      .resource_providers(0)
      .total_resources();

  EXPECT_EQ(v1::Resources(resource), responseResources);
}


// This test verifies that if a resource provider becomes disconnected, it will
// not be reported by `GET_AGENT` calls.
TEST_P(MasterAPITest, GetAgentsDisconnectedResourceProvider)
{
  Clock::pause();

  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start one agent.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->resource_providers().providers().empty());

  // Start a resource provider.
  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  Owned<v1::TestResourceProvider> resourceProvider(
    new v1::TestResourceProvider(info, v1::Resources()));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider->start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_FALSE(updateSlaveMessage->resource_providers().providers().empty());

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_AGENTS);

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());
    ASSERT_EQ(1, v1Response->get_agents().agents(0).resource_providers_size());

    const mesos::v1::ResourceProviderInfo& responseInfo =
      v1Response->get_agents()
        .agents(0)
        .resource_providers(0)
        .resource_provider_info();

    EXPECT_EQ(info.type(), responseInfo.type());
    EXPECT_EQ(info.name(), responseInfo.name());
    EXPECT_TRUE(responseInfo.has_id());
  }

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Disconnect the resource provider.
  resourceProvider.reset();

  // Wait until the agent's resources have been updated to exclude the
  // resource provider.
  AWAIT_READY(updateSlaveMessage);
  ASSERT_TRUE(updateSlaveMessage->resource_providers().providers().empty());

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_AGENTS);

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());
    EXPECT_TRUE(
        v1Response->get_agents().agents(0).resource_providers().empty());
  }
}


TEST_P(MasterAPITest, GetFlags)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_FLAGS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_FLAGS, v1Response->type());
}


TEST_P(MasterAPITest, GetFrameworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_FRAMEWORKS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_FRAMEWORKS, v1Response->type());

  v1::master::Response::GetFrameworks frameworks =
      v1Response->get_frameworks();

  ASSERT_EQ(1, frameworks.frameworks_size());
  ASSERT_EQ("default", frameworks.frameworks(0).framework_info().name());
  ASSERT_EQ("*", frameworks.frameworks(0).framework_info().roles(0));
  ASSERT_FALSE(frameworks.frameworks(0).framework_info().checkpoint());
  ASSERT_TRUE(frameworks.frameworks(0).active());
  ASSERT_TRUE(frameworks.frameworks(0).connected());
  ASSERT_FALSE(frameworks.frameworks(0).recovered());

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetHealth)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_HEALTH);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_HEALTH, v1Response->type());
  ASSERT_TRUE(v1Response->get_health().healthy());
}


TEST_P(MasterAPITest, GetVersion)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_VERSION);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_VERSION, v1Response->type());

  ASSERT_EQ(MESOS_VERSION,
            v1Response->get_version().version_info().version());
}


TEST_P(MasterAPITest, GetMetrics)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  Duration timeout = Seconds(5);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_METRICS);
  v1Call.mutable_get_metrics()->mutable_timeout()->set_nanoseconds(
      timeout.ns());

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_METRICS, v1Response->type());

  hashmap<string, double> metrics;

  foreach (const v1::Metric& metric,
           v1Response->get_metrics().metrics()) {
    ASSERT_TRUE(metric.has_value());
    metrics[metric.name()] = metric.value();
  }

  // Verifies that the response metrics is not empty.
  ASSERT_LE(0u, metrics.size());
}


TEST_P(MasterAPITest, GetExecutors)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID so we can use it to verify GET_EXECUTORS API
  // call.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

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
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());
  EXPECT_TRUE(status->has_executor_id());
  EXPECT_EQ(exec.id, status->executor_id());

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_EXECUTORS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_EXECUTORS, v1Response->type());
  ASSERT_EQ(1, v1Response->get_executors().executors_size());

  ASSERT_EQ(evolve(slaveId),
            v1Response->get_executors().executors(0).agent_id());

  v1::ExecutorInfo executorInfo =
    v1Response->get_executors().executors(0).executor_info();

  ASSERT_EQ(evolve(exec.id), executorInfo.executor_id());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetState)
{
  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_STATE);

  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  ASSERT_FALSE(offers->empty());

  ContentType contentType = GetParam();

  {
    // GetState before task launch and check we have one framework, one agent
    // and zero tasks/executors.
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    const v1::master::Response::GetState& getState = v1Response->get_state();
    ASSERT_EQ(1, getState.get_frameworks().frameworks_size());
    ASSERT_EQ(1, getState.get_agents().agents_size());
    ASSERT_TRUE(getState.get_tasks().tasks().empty());
    ASSERT_TRUE(getState.get_executors().executors().empty());
  }

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  const string checkCommandValue = "exit 0";

  // Add a health check to the task.
  {
    CommandInfo healthCommand;
    healthCommand.set_value(checkCommandValue);

    HealthCheck healthCheck;

    healthCheck.set_type(HealthCheck::COMMAND);
    healthCheck.mutable_command()->CopyFrom(healthCommand);
    healthCheck.set_delay_seconds(1);
    healthCheck.set_interval_seconds(1);
    healthCheck.set_timeout_seconds(1);
    healthCheck.set_consecutive_failures(1);
    healthCheck.set_grace_period_seconds(1);

    task.mutable_health_check()->CopyFrom(healthCheck);
  }

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(
        StatusUpdateAcknowledgementMessage(),
        Eq(master.get()->pid),
        Eq(slave.get()->pid));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(execDriver);
  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(acknowledgement);

  {
    // GetState after task launch and check we have a running task.
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    const v1::master::Response::GetState& getState = v1Response->get_state();
    ASSERT_EQ(1, getState.get_tasks().tasks_size());
    ASSERT_TRUE(getState.get_tasks().completed_tasks().empty());

    // Confirm that the health check definition is included.
    ASSERT_TRUE(getState.get_tasks().tasks(0).has_health_check());
    ASSERT_EQ(
        v1::HealthCheck::COMMAND,
        getState.get_tasks().tasks(0).health_check().type());
    ASSERT_EQ(
        checkCommandValue,
        getState.get_tasks().tasks(0).health_check().command().value());
    ASSERT_EQ(1, getState.get_tasks().tasks(0).health_check().delay_seconds());
    ASSERT_EQ(
        1,
        getState.get_tasks().tasks(0).health_check().interval_seconds());
    ASSERT_EQ(
        1,
        getState.get_tasks().tasks(0).health_check().timeout_seconds());
    ASSERT_EQ(
        1u,
        getState.get_tasks().tasks(0).health_check().consecutive_failures());
    ASSERT_EQ(
        1,
        getState.get_tasks().tasks(0).health_check().grace_period_seconds());
  }

  acknowledgement = FUTURE_PROTOBUF(
      StatusUpdateAcknowledgementMessage(),
      Eq(master.get()->pid),
      Eq(slave.get()->pid));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  // Send a terminal update so that the task transitions to completed.
  TaskStatus status3;
  status3.mutable_task_id()->CopyFrom(task.task_id());
  status3.set_state(TASK_FINISHED);

  execDriver.get()->sendStatusUpdate(status3);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  AWAIT_READY(acknowledgement);

  {
    // GetState after task finished and check we have a completed task.
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    const v1::master::Response::GetState& getState = v1Response->get_state();
    ASSERT_EQ(1, getState.get_tasks().completed_tasks_size());
    ASSERT_TRUE(getState.get_tasks().tasks().empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetTasksNoRunningTask)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_TASKS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_TASKS, v1Response->type());

  ASSERT_TRUE(v1Response->get_tasks().pending_tasks().empty());
  ASSERT_TRUE(v1Response->get_tasks().tasks().empty());
  ASSERT_TRUE(v1Response->get_tasks().completed_tasks().empty());
  ASSERT_TRUE(v1Response->get_tasks().orphan_tasks().empty());
}


// This test verifies that the GetTasks v1 API call returns responses correctly
// when the task transitions from being active to completed.
TEST_P(MasterAPITest, GetTasks)
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(
        StatusUpdateAcknowledgementMessage(),
        Eq(master.get()->pid),
        Eq(slave.get()->pid));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(execDriver);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());
  EXPECT_TRUE(status->has_executor_id());
  EXPECT_EQ(exec.id, status->executor_id());

  AWAIT_READY(acknowledgement);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_TASKS);

  ContentType contentType = GetParam();

  {
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_TASKS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_tasks().tasks().size());
    ASSERT_EQ(v1::TaskState::TASK_RUNNING,
              v1Response->get_tasks().tasks(0).state());
    ASSERT_EQ("test", v1Response->get_tasks().tasks(0).name());
    ASSERT_EQ("1", v1Response->get_tasks().tasks(0).task_id().value());
  }

  acknowledgement = FUTURE_PROTOBUF(
      StatusUpdateAcknowledgementMessage(),
      Eq(master.get()->pid),
      Eq(slave.get()->pid));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  // Send a terminal update so that the task transitions to completed.
  TaskStatus status3;
  status3.mutable_task_id()->CopyFrom(task.task_id());
  status3.set_state(TASK_FINISHED);

  execDriver.get()->sendStatusUpdate(status3);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  AWAIT_READY(acknowledgement);

  {
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_TASKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_tasks().tasks().empty());
    ASSERT_EQ(1, v1Response->get_tasks().completed_tasks().size());
    ASSERT_EQ(v1::TaskState::TASK_FINISHED,
              v1Response->get_tasks().completed_tasks(0).state());
    ASSERT_EQ("test", v1Response->get_tasks().completed_tasks(0).name());
    ASSERT_EQ(
        "1",
        v1Response->get_tasks().completed_tasks(0).task_id().value());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetLoggingLevel)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_LOGGING_LEVEL);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_LOGGING_LEVEL, v1Response->type());
  ASSERT_LE(0, FLAGS_v);
  ASSERT_EQ(
      v1Response->get_logging_level().level(),
      static_cast<uint32_t>(FLAGS_v));
}


// Test the logging level toggle and revert after specific toggle duration.
TEST_P(MasterAPITest, SetLoggingLevel)
{
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to set the logging level.
    mesos::ACL::SetLogLevel* acl = flags.acls->add_set_log_level();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_level()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = this->StartMaster(flags);
  ASSERT_SOME(master);

  // We capture the original logging level first; it would be used to verify
  // the logging level revert works.
  uint32_t originalLevel = static_cast<uint32_t>(FLAGS_v);

  // Send request to master to toggle the logging level.
  uint32_t toggleLevel = originalLevel + 1;
  Duration toggleDuration = Seconds(60);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SET_LOGGING_LEVEL);
  v1::master::Call_SetLoggingLevel* setLoggingLevel =
    v1Call.mutable_set_logging_level();
  setLoggingLevel->set_level(toggleLevel);
  setLoggingLevel->mutable_duration()->set_nanoseconds(toggleDuration.ns());

  ContentType contentType = GetParam();

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> v1Response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, v1Response);
  }

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> v1Response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, v1Response);

    AWAIT_READY(v1Response);
    ASSERT_EQ(toggleLevel, static_cast<uint32_t>(FLAGS_v));
  }

  // Speedup the logging level revert.
  Clock::pause();
  Clock::advance(toggleDuration);
  Clock::settle();

  // Verifies the logging level reverted successfully.
  ASSERT_EQ(originalLevel, static_cast<uint32_t>(FLAGS_v));
  Clock::resume();
}


// This test verifies if we can retrieve the file listing for a directory
// in the master.
TEST_P_TEMP_DISABLED_ON_WINDOWS(MasterAPITest, ListFiles)
{
  Files files;

  ASSERT_SOME(os::mkdir("1/2"));
  ASSERT_SOME(os::mkdir("1/3"));
  ASSERT_SOME(os::write("1/two", "two"));

  AWAIT_EXPECT_READY(files.attach("1", "one"));

  // Get the `FileInfo` for "1/two" file.
  struct stat s;
  ASSERT_EQ(0, stat("1/two", &s));
  FileInfo file = protobuf::createFileInfo("one/two", s);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::LIST_FILES);
  v1Call.mutable_list_files()->set_path("one/");

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::LIST_FILES, v1Response->type());
  ASSERT_EQ(3, v1Response->list_files().file_infos().size());
  ASSERT_EQ(evolve(file), v1Response->list_files().file_infos(2));
}


// This test verifies that the client will receive a `NotFound` response when it
// tries to make a `LIST_FILES` call with an invalid path.
TEST_P(MasterAPITest, ListFilesInvalidPath)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::LIST_FILES);
  v1Call.mutable_list_files()->set_path("five/");

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
}


TEST_P(MasterAPITest, GetRoles)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "role1";
  masterFlags.weights = "role1=2.5";

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources =
    "cpus(role1):0.5;mem(role1):512;ports(role1):[31000-31001];"
    "disk(role1):1024;gpus(role1):0";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_ROLES);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_ROLES, v1Response->type());

  ASSERT_EQ(2, v1Response->get_roles().roles().size());
  EXPECT_EQ("role1", v1Response->get_roles().roles(1).name());
  EXPECT_EQ(2.5, v1Response->get_roles().roles(1).weight());

  EXPECT_EQ(
      CHECK_NOTERROR(v1::Resources::parse(
          "cpus:0.5; disk:1024; mem:512")),
      v1Response->get_roles().roles(1).resources());

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetOperations)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to view any role.
    mesos::ACL::ViewRole* acl = masterFlags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start one agent.
  Future<UpdateSlaveMessage> updateAgentMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(updateAgentMessage);

  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  v1::TestResourceProvider resourceProvider(
      info,
      v1::createDiskResource(
          "200",
          "*",
          None(),
          None(),
          v1::createDiskSourceRaw(None(), "profile")));

  // Start and register resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  updateAgentMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  const ContentType contentType = GetParam();

  resourceProvider.start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateAgentMessage);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_OPERATIONS);

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_TRUE(v1Response->get_operations().operations().empty());

  // Start a framework to operate on offers.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // We settle here to make sure that the framework has been authenticated
  // before advancing the clock. Otherwise we would run into a authentication
  // timeout due to the large allocation interval (1000s) of this fixture.
  Clock::settle();

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers->front();

  Option<Resource> rawDisk;

  foreach (const Resource& resource, offer.resources()) {
    if (resource.has_provider_id() &&
        resource.has_disk() &&
        resource.disk().has_source() &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      rawDisk = resource;
      break;
    }
  }

  ASSERT_SOME(rawDisk);

  // The operation is still pending when we receive this event.
  Future<mesos::v1::resource_provider::Event::ApplyOperation> operation;
  EXPECT_CALL(*resourceProvider.process, applyOperation(_))
    .WillOnce(FutureArg<0>(&operation));

  // Start an operation.
  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(rawDisk.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(operation);

  v1Response = post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_EQ(1, v1Response->get_operations().operations_size());
  EXPECT_EQ(
      operation->framework_id(),
      v1Response->get_operations().operations(0).framework_id());
  EXPECT_EQ(
      evolve(updateAgentMessage->slave_id()),
      v1Response->get_operations().operations(0).agent_id());
  EXPECT_EQ(
      operation->info(), v1Response->get_operations().operations(0).info());
  EXPECT_EQ(
      operation->operation_uuid(),
      v1Response->get_operations().operations(0).uuid());

  // Default principal 2 should not be able to see any operations.
  v1Response =
    post(master.get()->pid, v1Call, contentType, DEFAULT_CREDENTIAL_2);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_TRUE(v1Response->get_operations().operations().empty());

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetMaster)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_MASTER);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_MASTER, v1Response->type());

  const mesos::v1::MasterInfo& masterInfo =
    v1Response->get_master().master_info();

  ASSERT_EQ(evolve(masterFlags.domain.get()), masterInfo.domain());
  ASSERT_EQ(master.get()->getMasterInfo().ip(), masterInfo.ip());

  const v1::master::Response::GetMaster getMaster = v1Response->get_master();

  ASSERT_TRUE(getMaster.has_start_time());
  ASSERT_TRUE(Clock::now().secs() > getMaster.start_time());

  ASSERT_TRUE(getMaster.has_elected_time());
  ASSERT_TRUE(Clock::now().secs() > getMaster.elected_time());
}


// This test verifies that an operator can reserve available resources through
// the `RESERVE_RESOURCES` call.
TEST_P(MasterAPITest, ReserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::RESERVE_RESOURCES);

  v1::master::Call::ReserveResources* reserveResources =
    v1Call.mutable_reserve_resources();

  reserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId.get()));
  reserveResources->mutable_resources()->CopyFrom(evolve(dynamicallyReserved));

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Accepted().status, response);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test verifies that an operator can update existing reservation through
// the `RESERVE_RESOURCES` call.
TEST_P(MasterAPITest, ReservationUpdate)
{
  ContentType contentType = GetParam();

  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  ASSERT_SOME(slave);
  AWAIT_READY(__recover);

  // It's actually impossible to construct `Resources` from the contents
  // of `state["reserved_resources"]`, so instead we just compare against
  // the expected JSON below.
  JSON::Value resourcesJson = JSON::parse(R"_(
        {
            "cpus": 1.0,
            "disk": 0.0,
            "gpus": 0.0,
            "mem": 10.0
        }
      )_").get();

  Resources unreserved = Resources::parse("cpus:1;mem:10").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        "role", DEFAULT_CREDENTIAL.principal()));

  Resources dynamicallyReservedToFoo =
    dynamicallyReserved.pushReservation(createDynamicReservationInfo(
        "role/foo", DEFAULT_CREDENTIAL.principal()));

  Resources dynamicallyReservedToBar =
    dynamicallyReserved.pushReservation(createDynamicReservationInfo(
        "role/bar", DEFAULT_CREDENTIAL.principal()));

  // Helper function to attempt a reservation update from a given `source`
  // to a given reservation using an operator API call.
  //
  // Note that RESERVE_RESOURCES call does not wait for the agent to apply
  // the operation, hence this function might return before the agent changes
  // its state!
  auto attemptReservation = [&master, &slaveId, &contentType](
      const Resources& source,
      const Resources& resources,
      const std::string& expectedResponseStatus) {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::RESERVE_RESOURCES);
    v1::master::Call::ReserveResources* reserveResources =
      v1Call.mutable_reserve_resources();

    reserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId.get()));
    reserveResources->mutable_source()->CopyFrom(evolve(source));
    reserveResources->mutable_resources()->CopyFrom(evolve(resources));

    Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(expectedResponseStatus, response);
  };

  // Helper function to verify that the resources on the agent are
  // indeed reserved to the specified role.
  auto verifyReservation = [&slave, &resourcesJson](
      const std::string& intendedRole) {
    Future<http::Response> response = http::get(
        slave.get()->pid,
        "state",
        None(), // query
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> json = JSON::parse(response->body);
    ASSERT_SOME(json);

    Result<JSON::Object> reservations =
      json->as<JSON::Object>().at<JSON::Object>("reserved_resources");

    ASSERT_SOME(reservations);
    EXPECT_EQ(reservations->values.size(), 1u);

    foreachpair (
        const std::string& role,
        const JSON::Value& reservation,
        reservations->values) {
      EXPECT_EQ(role, intendedRole);
      EXPECT_EQ(resourcesJson, reservation);
    }
  };

  // Setup done, actual test can start now.
  const std::string conflict = http::Conflict().status;
  const std::string accepted = http::Accepted().status;

  // Should fail, since there are no resources that are dynamically
  // reserved to `role/bar`.
  attemptReservation(
      dynamicallyReservedToBar,
      dynamicallyReservedToFoo,
      conflict);

  // Reserve unreserved resources to `role/foo`.
  attemptReservation(
      unreserved,
      dynamicallyReservedToFoo,
      accepted);

  // Wait for the agent to apply operation.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  verifyReservation("role/foo");

  // Should fail again, since there are still no resources that are
  // dynamically reserved to `role/bar`.
  attemptReservation(
      dynamicallyReservedToBar,
      dynamicallyReservedToFoo,
      conflict);

  // Update reservation from `role/foo` to `role/bar`.
  attemptReservation(
      dynamicallyReservedToFoo,
      dynamicallyReservedToBar,
      accepted);

  // Wait for the agent to apply operation.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  verifyReservation("role/bar");
}


// This test verifies that an operator can unreserve reserved resources through
// the `UNRESERVE_RESOURCES` call.
TEST_P(MasterAPITest, UnreserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::RESERVE_RESOURCES);

  v1::master::Call::ReserveResources* reserveResources =
    v1Call.mutable_reserve_resources();

  reserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId.get()));
  reserveResources->mutable_resources()->CopyFrom(evolve(dynamicallyReserved));

  ContentType contentType = GetParam();

  Future<http::Response> reserveResponse = http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Accepted().status, reserveResponse);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  v1Call.set_type(v1::master::Call::UNRESERVE_RESOURCES);

  v1::master::Call::UnreserveResources* unreserveResources =
    v1Call.mutable_unreserve_resources();

  unreserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId.get()));

  unreserveResources->mutable_resources()->CopyFrom(
      evolve(dynamicallyReserved));

  Future<http::Response> unreserveResponse = http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Accepted().status, unreserveResponse);

  // Summon an offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.reviveOffers();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // Verifies if the resources are unreserved.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// Test updates a maintenance schedule and verifies it saved via query.
TEST_P(MasterAPITest, UpdateAndGetMaintenanceSchedule)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to update any maintenance schedule.
    mesos::ACL::UpdateMaintenanceSchedule* acl =
      flags.acls->add_update_maintenance_schedules();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Default principal 2 is not allowed to view any maintenance schedule.
    mesos::ACL::GetMaintenanceSchedule* acl =
      flags.acls->add_get_maintenance_schedules();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Generate `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  machine1.set_hostname("Machine1");
  machine2.set_ip("0.0.0.2");

  // Try to schedule maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, createUnavailability(Clock::now()))});
  v1::maintenance::Schedule v1Schedule = evolve(schedule);

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  ContentType contentType = GetParam();

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1UpdateScheduleCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1UpdateScheduleCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Query maintenance schedule.
  v1::master::Call v1GetScheduleCall;
  v1GetScheduleCall.set_type(v1::master::Call::GET_MAINTENANCE_SCHEDULE);

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1GetScheduleCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Future<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    AWAIT_READY(v1Response);
    v1::maintenance::Schedule schedule =
      v1Response->get_maintenance_schedule().schedule();
    ASSERT_TRUE(schedule.windows().empty());
  }

  {
    Future<v1::master::Response> response =
      post(master.get()->pid, v1GetScheduleCall, contentType);

    AWAIT_READY(response);
    ASSERT_TRUE(response->IsInitialized());
    ASSERT_EQ(
        v1::master::Response::GET_MAINTENANCE_SCHEDULE,
        response->type());

    // Verify maintenance schedule matches the expectation.
    v1::maintenance::Schedule schedule =
        response->get_maintenance_schedule().schedule();
    ASSERT_EQ(1, schedule.windows().size());
    ASSERT_EQ(2, schedule.windows(0).machine_ids().size());
    ASSERT_EQ("Machine1", schedule.windows(0).machine_ids(0).hostname());
    ASSERT_EQ("0.0.0.2", schedule.windows(0).machine_ids(1).ip());
  }
}


// Test queries for machine maintenance status.
TEST_P(MasterAPITest, GetMaintenanceStatus)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to view any maintenance status.
    mesos::ACL::GetMaintenanceStatus* acl =
      flags.acls->add_get_maintenance_statuses();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  ContentType contentType = GetParam();
  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  // Generate `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  machine1.set_hostname("Machine1");
  machine2.set_ip("0.0.0.2");

  // Try to schedule maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, createUnavailability(Clock::now()))});
  v1::maintenance::Schedule v1Schedule = evolve(schedule);

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  Future<http::Response> v1UpdateScheduleResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, v1UpdateScheduleResponse);

  // Query maintenance status.
  v1::master::Call v1GetStatusCall;
  v1GetStatusCall.set_type(v1::master::Call::GET_MAINTENANCE_STATUS);

  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1GetStatusCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    v1::maintenance::ClusterStatus status =
      v1Response->get_maintenance_status().status();
    ASSERT_TRUE(status.draining_machines().empty());
    ASSERT_TRUE(status.down_machines().empty());
  }

  {
    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1GetStatusCall, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_MAINTENANCE_STATUS, v1Response->type());

    // Verify maintenance status matches the expectation.
    v1::maintenance::ClusterStatus status =
      v1Response->get_maintenance_status().status();
    ASSERT_EQ(2, status.draining_machines().size());
    ASSERT_TRUE(status.down_machines().empty());
  }
}


// Test start machine maintenance and stop machine maintenance APIs.
// In this test case, we start maintenance on a machine and stop maintenance,
// and then verify that the associated maintenance window disappears.
TEST_P(MasterAPITest, StartAndStopMaintenance)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to start maintenance in any machine.
    mesos::ACL::StartMaintenance* acl = flags.acls->add_start_maintenances();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Default principal 2 is not allowed to stop maintenance in any machine.
    mesos::ACL::StopMaintenance* acl = flags.acls->add_stop_maintenances();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  ContentType contentType = GetParam();
  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  // Generate `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  MachineID machine3;
  machine1.set_hostname("Machine1");
  machine2.set_ip("0.0.0.2");
  machine3.set_hostname("Machine3");
  machine3.set_ip("0.0.0.3");

  // Try to schedule maintenance on unscheduled machines.
  Unavailability unavailability = createUnavailability(Clock::now());
  maintenance::Schedule schedule = createSchedule({
      createWindow({machine1, machine2}, unavailability),
      createWindow({machine3}, unavailability)
  });

  v1::maintenance::Schedule v1Schedule = evolve(schedule);

  {
    v1::master::Call v1UpdateScheduleCall;
    v1UpdateScheduleCall.set_type(
        v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
    v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
      v1UpdateScheduleCall.mutable_update_maintenance_schedule();
    maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1UpdateScheduleCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Start maintenance on machine3.
  v1::master::Call v1StartMaintenanceCall;
  v1StartMaintenanceCall.set_type(v1::master::Call::START_MAINTENANCE);
  v1::master::Call_StartMaintenance* startMaintenance =
    v1StartMaintenanceCall.mutable_start_maintenance();
  startMaintenance->add_machines()->CopyFrom(evolve(machine3));

  {
    headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1StartMaintenanceCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1StartMaintenanceCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Stop maintenance on machine3.
  v1::master::Call v1StopMaintenanceCall;
  v1StopMaintenanceCall.set_type(v1::master::Call::STOP_MAINTENANCE);
  v1::master::Call_StopMaintenance* stopMaintenance =
    v1StopMaintenanceCall.mutable_stop_maintenance();
  stopMaintenance->add_machines()->CopyFrom(evolve(machine3));

  {
    headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1StopMaintenanceCall),
       stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1StopMaintenanceCall),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Query maintenance schedule.
  v1::master::Call v1GetScheduleCall;
  v1GetScheduleCall.set_type(v1::master::Call::GET_MAINTENANCE_SCHEDULE);

  Future<v1::master::Response> v1GetScheduleResponse =
    post(master.get()->pid, v1GetScheduleCall, contentType);

  AWAIT_READY(v1GetScheduleResponse);
  ASSERT_TRUE(v1GetScheduleResponse->IsInitialized());
  ASSERT_EQ(
      v1::master::Response::GET_MAINTENANCE_SCHEDULE,
      v1GetScheduleResponse->type());

  // Check that only one maintenance window remains.
  v1::maintenance::Schedule respSchedule =
    v1GetScheduleResponse->get_maintenance_schedule().schedule();
  ASSERT_EQ(1, respSchedule.windows().size());
  ASSERT_EQ(2, respSchedule.windows(0).machine_ids().size());
  ASSERT_EQ("Machine1", respSchedule.windows(0).machine_ids(0).hostname());
  ASSERT_EQ("0.0.0.2", respSchedule.windows(0).machine_ids(1).ip());
}


// This test verifies that a subscriber can receive `AGENT_ADDED`
// and `AGENT_REMOVED` events.
TEST_P(MasterAPITest, SubscribeAgentEvents)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::MockMasterAPISubscriber subscriber;

  Future<v1::master::Event::Subscribed> subscribed;
  EXPECT_CALL(subscriber, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed));

  AWAIT_READY(subscriber.subscribe(master.get()->pid, contentType));
  AWAIT_READY(subscribed);

  const v1::master::Response::GetState& getState = subscribed->get_state();

  EXPECT_TRUE(getState.get_frameworks().frameworks().empty());
  EXPECT_TRUE(getState.get_agents().agents().empty());
  EXPECT_TRUE(getState.get_tasks().tasks().empty());
  EXPECT_TRUE(getState.get_executors().executors().empty());

  // Expect AGENT_ADDED message after starting an agent
  Future<v1::master::Event::AgentAdded> agentAdded;
  EXPECT_CALL(subscriber, agentAdded(_))
    .WillOnce(FutureArg<0>(&agentAdded));

  // Start one agent.
  Future<SlaveRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags flags = CreateSlaveFlags();
  flags.hostname = "host";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(agentRegisteredMessage);

  AWAIT_READY(agentAdded);

  SlaveID agentID = agentRegisteredMessage->slave_id();

  {
    const v1::master::Response::GetAgents::Agent& agent = agentAdded->agent();

    ASSERT_EQ("host", agent.agent_info().hostname());
    ASSERT_EQ(evolve(agentID), agent.agent_info().id());
    ASSERT_EQ(slave.get()->pid, agent.pid());
    ASSERT_EQ(MESOS_VERSION, agent.version());
    ASSERT_EQ(4, agent.total_resources_size());
  }

  // Expect AGENT_REMOVED message after agent removal.
  Future<v1::master::Event::AgentRemoved> agentRemoved;
  EXPECT_CALL(subscriber, agentRemoved(_))
    .WillOnce(FutureArg<0>(&agentRemoved));

  // Forcefully trigger a shutdown on the slave so that master will remove it.
  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(agentRemoved);

  ASSERT_EQ(evolve(agentID), agentRemoved->agent_id());
}


// This test verifies that no information about reservations and/or allocations
// is returned to unauthorized users in response to the GET_AGENTS call.
TEST_P(MasterAPITest, GetAgentsFiltering)
{
  master::Flags flags = CreateMasterFlags();

  const string roleSuperhero = "superhero";
  const string roleMuggle = "muggle";

  {
    mesos::ACL::ViewRole* acl = flags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->add_values(roleSuperhero);

    acl = flags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    mesos::ACL::ViewRole* acl = flags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->add_values(roleMuggle);

    acl = flags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Statically reserve some resources on the agent.
  slaveFlags.resources =
    "cpus(muggle):1;cpus(*):2;gpus(*):0;mem(muggle):1024;mem(*):1024;"
    "disk(muggle):1024;disk(*):1024;ports(muggle):[30000-30999];"
    "ports(*):[31000-32000]";

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(agentRegisteredMessage);
  const SlaveID& agentId = agentRegisteredMessage->slave_id();

  // Create dynamic reservation.
  {
    RepeatedPtrField<Resource> reservation =
      Resources::parse("cpus:1;mem:12")->pushReservation(
          createDynamicReservationInfo(
              roleSuperhero,
              DEFAULT_CREDENTIAL.principal()));

    Future<http::Response> response = process::http::post(
        master.get()->pid,
        "reserve",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        strings::format(
            "slaveId=%s&resources=%s",
            agentId,
            JSON::protobuf(reservation)).get());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Accepted().status, response);
  }

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_AGENTS);
  ContentType contentType = GetParam();

  // Default credential principal should only be allowed to see resources
  // which are reserved for the role 'superhero'.
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());

    // AgentInfo.resources is not passed through `convertResourceFormat()` so
    // its format is different.
    foreach (const v1::Resource& resource,
             v1Response->get_agents().agents(0).agent_info().resources()) {
      EXPECT_FALSE(resource.has_role());
      EXPECT_FALSE(resource.has_allocation_info());
      EXPECT_FALSE(resource.has_reservation());
      EXPECT_TRUE(resource.reservations().empty());
    }

    vector<RepeatedPtrField<v1::Resource>> resourceFields = {
      v1Response->get_agents().agents(0).total_resources(),
      v1Response->get_agents().agents(0).allocated_resources(),
      v1Response->get_agents().agents(0).offered_resources()
    };

    bool hasReservedResources = false;
    foreach (const RepeatedPtrField<v1::Resource>& resources, resourceFields) {
      foreach (const v1::Resource& resource, resources) {
        EXPECT_TRUE(resource.has_role());
        EXPECT_TRUE(roleSuperhero == resource.role() || "*" == resource.role());

        EXPECT_FALSE(resource.has_allocation_info());

        if (resource.role() != "*") {
          hasReservedResources = true;

          EXPECT_TRUE(resource.has_reservation());
          EXPECT_FALSE(resource.reservation().has_role());

          EXPECT_FALSE(resource.reservations().empty());
          foreach (const v1::Resource::ReservationInfo& reservation,
                   resource.reservations()) {
            EXPECT_EQ(roleSuperhero, reservation.role());
          }
        } else {
          EXPECT_FALSE(resource.has_reservation());
          EXPECT_TRUE(resource.reservations().empty());
        }
      }
    }
    EXPECT_TRUE(hasReservedResources);
  }

  // Default credential principal 2 should only be allowed to see resources
  // which are reserved for the role 'muggle'.
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

    Try<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());

    // AgentInfo.resources is not passed through `convertResourceFormat()` so
    // its format is different.
    foreach (const v1::Resource& resource,
             v1Response->get_agents().agents(0).agent_info().resources()) {
      EXPECT_FALSE(resource.has_role());
      EXPECT_FALSE(resource.has_allocation_info());
      EXPECT_FALSE(resource.has_reservation());
      if (resource.reservations_size() > 0) {
        foreach (const v1::Resource::ReservationInfo& reservation,
                 resource.reservations()) {
          EXPECT_EQ(roleMuggle, reservation.role());
        }
      }
    }

    vector<RepeatedPtrField<v1::Resource>> resourceFields = {
      v1Response->get_agents().agents(0).total_resources(),
      v1Response->get_agents().agents(0).allocated_resources(),
      v1Response->get_agents().agents(0).offered_resources()
    };

    bool hasReservedResources = false;
    foreach (const RepeatedPtrField<v1::Resource>& resources, resourceFields) {
      foreach (const v1::Resource& resource, resources) {
        EXPECT_TRUE(resource.has_role());
        EXPECT_TRUE(roleMuggle == resource.role() || "*" == resource.role());

        EXPECT_FALSE(resource.has_allocation_info());

        if (resource.role() != "*") {
          hasReservedResources = true;
          EXPECT_FALSE(resource.has_reservation());

          EXPECT_FALSE(resource.reservations().empty());
          foreach (const v1::Resource::ReservationInfo& reservation,
                   resource.reservations()) {
            EXPECT_EQ(roleMuggle, reservation.role());
          }
        } else {
          EXPECT_FALSE(resource.has_reservation());
          EXPECT_TRUE(resource.reservations().empty());
        }
      }
    }
    EXPECT_TRUE(hasReservedResources);
  }
}


// This test verifies that recovered but yet to reregister agents are returned
// in `recovered_agents` field of `GetAgents` response. Authorization is enabled
// to ensure that authorization-based filtering is able to handle recovered
// agents, whose resources are currently stored in the
// pre-reservation-refinement format (see MESOS-7851).
TEST_P_TEMP_DISABLED_ON_WINDOWS(MasterAPITest, GetRecoveredAgents)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  // This forces the authorizer to be initialized.
  {
    mesos::ACL::ViewRole* acl = masterFlags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  // Statically reserve some resources on the agent.
  slaveFlags.resources =
    "cpus(foo):1;cpus(*):2;gpus(*):0;mem(foo):1024;mem(*):1024;"
    "disk(foo):1024;disk(*):1024;ports(*):[31000-32000]";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  v1::AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

  // Create dynamic reservation.
  {
    RepeatedPtrField<Resource> reservation =
      Resources::parse("cpus:1;mem:12")->pushReservation(
          createDynamicReservationInfo(
              "bar",
              DEFAULT_CREDENTIAL.principal()));

    Future<http::Response> response = process::http::post(
        master.get()->pid,
        "reserve",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        strings::format(
            "slaveId=%s&resources=%s",
            agentId,
            JSON::protobuf(reservation)).get());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Accepted().status, response);
  }

  // Ensure that the agent is present in `GetAgent.agents` while
  // `GetAgents.recovered_agents` is empty.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_AGENTS);

    ContentType contentType = GetParam();

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());
    ASSERT_EQ(agentId,
              v1Response->get_agents().agents(0).agent_info().id());
    ASSERT_TRUE(v1Response->get_agents().recovered_agents().empty());
  }

  // Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Ensure that the agent is present in `GetAgents.recovered_agents`
  // while `GetAgents.agents` is empty.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_AGENTS);

    ContentType contentType = GetParam();

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_TRUE(v1Response->get_agents().agents().empty());
    ASSERT_EQ(1, v1Response->get_agents().recovered_agents_size());
    ASSERT_EQ(agentId, v1Response->get_agents().recovered_agents(0).id());
  }

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Start the agent to make it reregister with the master.
  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // After the agent has successfully reregistered with the master,
  // the `GetAgents.recovered_agents` field would be empty.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_AGENTS);

    ContentType contentType = GetParam();

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_agents().agents_size());
    ASSERT_TRUE(v1Response->get_agents().recovered_agents().empty());
  }
}


// This test tries to verify that a client subscribed to the 'api/v1'
// endpoint is able to receive `TASK_ADDED`/`TASK_UPDATED` events.
TEST_P(MasterAPITest, Subscribe)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(subscribed);

  // Launch a task using the scheduler. This should result in a `TASK_ADDED`
  // event when the task is launched followed by a `TASK_UPDATED` event after
  // the task transitions to running state.
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  // Create event stream after seeing first offer but before first task is
  // launched. We should see one framework, one agent and zero task/executor.
  v1::MockMasterAPISubscriber subscriber;

  Future<v1::master::Event::Subscribed> subscribedToMasterEvents;
  EXPECT_CALL(subscriber, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribedToMasterEvents));

  AWAIT_READY(subscriber.subscribe(master.get()->pid, contentType));
  AWAIT_READY(subscribedToMasterEvents);

  const v1::master::Response::GetState& getState =
    subscribedToMasterEvents->get_state();

  EXPECT_EQ(1, getState.get_frameworks().frameworks_size());
  EXPECT_EQ(1, getState.get_agents().agents_size());
  EXPECT_TRUE(getState.get_tasks().tasks().empty());
  EXPECT_TRUE(getState.get_executors().executors().empty());

  // Expect TASK_ADDED and TASK_UPDATED events, in sequence.
  Sequence masterEventsSequence;

  Future<v1::master::Event::TaskAdded> taskAdded;
  EXPECT_CALL(subscriber, taskAdded(_))
    .InSequence(masterEventsSequence)
    .WillOnce(FutureArg<0>(&taskAdded));

  // There should be only one taskUpdated event
  Future<v1::master::Event::TaskUpdated> taskUpdated;
  EXPECT_CALL(subscriber, taskUpdated(_))
    .InSequence(masterEventsSequence)
    .WillOnce(FutureArg<0>(&taskUpdated));

  const v1::Offer& offer = offers->offers(0);

  TaskInfo task = createTask(devolve(offer), "", executorId);

  const string checkCommandValue = "exit 0";

  // Add a health check to the task.
  {
    CommandInfo healthCommand;
    healthCommand.set_value(checkCommandValue);

    HealthCheck healthCheck;

    healthCheck.set_type(HealthCheck::COMMAND);
    healthCheck.mutable_command()->CopyFrom(healthCommand);

    task.mutable_health_check()->CopyFrom(healthCheck);
  }

  Future<Nothing> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureSatisfy(&update));

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  EXPECT_CALL(*executor, acknowledged(_, _));

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::ACCEPT);

    call.mutable_framework_id()->CopyFrom(frameworkId);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);

    operation->mutable_launch()->add_task_infos()->CopyFrom(evolve(task));

    mesos.send(call);
  }

  AWAIT_READY(taskAdded);
  AWAIT_READY(update);

  ASSERT_EQ(evolve(task.task_id()), taskAdded->task().task_id());
  ASSERT_TRUE(taskAdded->task().has_health_check());
  ASSERT_EQ(
      v1::HealthCheck::COMMAND,
      taskAdded->task().health_check().type());
  ASSERT_EQ(
      checkCommandValue,
      taskAdded->task().health_check().command().value());

  AWAIT_READY(taskUpdated);

  ASSERT_EQ(v1::TASK_RUNNING, taskUpdated->state());
  ASSERT_EQ(v1::TASK_RUNNING, taskUpdated->status().state());
  ASSERT_EQ(evolve(task.task_id()), taskUpdated->status().task_id());

  Future<Nothing> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureSatisfy(&update2));

  // After we advance the clock, the task status update manager would retry the
  // `TASK_RUNNING` update. Since, the state of the task is not changed, this
  // should not result in another `TASK_UPDATED` event.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  AWAIT_READY(update2);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// Helper functor for SubscribersReceiveHealthUpdates test.
// NOTE: We cannot use a lambda due to testing::ResultOf requiring result_type.
struct IsFailedHealthCheckTaskUpdate
{
  TaskID taskId;
  using result_type = bool;
  bool operator() (const v1::master::Event::TaskUpdated& event) const {
    const v1::TaskStatus& status = event.status();
    return status.task_id() == evolve(taskId) &&
           status.state() == v1::TASK_RUNNING &&
           status.reason() ==
             v1::TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED &&
           !status.healthy();
  };
};


// This test verifies that subscribers eventually receive events with the
// task health information.
TEST_P(MasterAPITest, SubscribersReceiveHealthUpdates)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Connect the scheduler.
  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  // Subscribe the scheduler. This will trigger a resource offer.
  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));
  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Subscribe to the master's event stream via the v1 API.
  v1::MockMasterAPISubscriber subscriber;
  AWAIT_READY(subscriber.subscribe(master.get()->pid, contentType));


  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId(offer.agent_id());

  TaskInfo task = createTask(devolve(offer), SLEEP_COMMAND(10000));

  // Expect taskUpdated event caused by a failed health check.
  Future<Nothing> taskUpdateWithFailedHealthCheckObserved;
  EXPECT_CALL(
    subscriber,
    taskUpdated(
      testing::ResultOf(IsFailedHealthCheckTaskUpdate{task.task_id()}, true)))
    .WillOnce(FutureSatisfy(&taskUpdateWithFailedHealthCheckObserved));

  // This describes a single health check that will fail.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(80);
  healthCheck.mutable_http()->set_path("/help");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(1000);
  healthCheck.set_grace_period_seconds(0);
  task.mutable_health_check()->CopyFrom(healthCheck);

  EXPECT_CALL(*scheduler, update(_, _))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({evolve(task)})}));

  AWAIT_READY(taskUpdateWithFailedHealthCheckObserved);

  // Kill task before test tear-down to avoid race between scheduler
  // tear-down and scheduler getting/acknowledging status update.
  //
  // TODO(asekretenko): Check if we still need this.
  Future<Nothing> killed;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_KILLED)))
    .WillOnce(FutureSatisfy(&killed));

  mesos.send(v1::createCallKill(frameworkId, evolve(task.task_id()), agentId));

  AWAIT_READY(killed);
}


// This test verifies that subscribing to the 'api/v1' endpoint between
// a master failover and an agent reregistration won't cause the master
// to crash, and frameworks recovered through agent reregistration will be
// broadcast to subscribers. See MESOS-8601 and MESOS-9785.
TEST_P(MasterAPITest, MasterFailover)
{
  ContentType contentType = GetParam();

  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  Owned<v1::scheduler::TestMesos> mesos(new v1::scheduler::TestMesos(
      master.get()->pid,
      contentType,
      scheduler));

  AWAIT_READY(subscribed);
  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // Settle the clock to get the first offer.
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());
  const v1::AgentID& agentId = offers->offers(0).agent_id();

  // Launch a task using the scheduler. This should result in a
  // `TASK_STARTING` followed by a `TASK_RUNNING` status update.
  Future<v1::scheduler::Event::Update> taskStarting;
  Future<v1::scheduler::Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&taskStarting),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&taskRunning),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  v1::TaskInfo task = v1::createTask(
      agentId,
      offers->offers(0).resources(),
      "sleep 1000");

  mesos->send(v1::createCallAccept(
      frameworkId,
      offers->offers(0),
      {v1::LAUNCH({task})}));

  AWAIT_READY(taskStarting);
  EXPECT_EQ(v1::TASK_STARTING, taskStarting->status().state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(v1::TASK_RUNNING, taskRunning->status().state());

  // Stop the scheduler and shutdown the master.
  mesos.reset();
  master->reset();

  Clock::settle();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  detector.appoint(master.get()->pid);

  v1::MockMasterAPISubscriber subscriber;

  Future<v1::master::Event::Subscribed> subscribedToMasterAPIEvents;
  EXPECT_CALL(subscriber, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribedToMasterAPIEvents));

  // The agent re-registration should result in an `AGENT_ADDED` event,
  // a `FRAMEWORK_ADDED` event and a `TASK_ADDED` event in order.
  Sequence masterEventsSequence;

  Future<v1::master::Event::AgentAdded> agentAdded;
  EXPECT_CALL(subscriber, agentAdded(_))
    .InSequence(masterEventsSequence)
    .WillOnce(FutureArg<0>(&agentAdded));

  Future<v1::master::Event::FrameworkAdded> frameworkAdded;
  EXPECT_CALL(subscriber, frameworkAdded(_))
    .InSequence(masterEventsSequence)
    .WillOnce(FutureArg<0>(&frameworkAdded));

  Future<v1::master::Event::TaskAdded> taskAdded;
  EXPECT_CALL(subscriber, taskAdded(_))
    .InSequence(masterEventsSequence)
    .WillOnce(FutureArg<0>(&taskAdded));

  // Create event stream after the master failover but before the agent
  // re-registration. We should see no framework, agent, task and
  // executor at all.
  AWAIT_READY(subscriber.subscribe(master.get()->pid, contentType));
  AWAIT_READY(subscribedToMasterAPIEvents);

  const v1::master::Response::GetState& getState =
    subscribedToMasterAPIEvents->get_state();

  EXPECT_EQ(0, getState.get_frameworks().frameworks_size());
  EXPECT_EQ(0, getState.get_agents().agents_size());
  EXPECT_EQ(0, getState.get_tasks().tasks_size());
  EXPECT_EQ(0, getState.get_executors().executors_size());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Advance the clock to trigger agent re-registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveReregisteredMessage);

  AWAIT_READY(agentAdded);
  EXPECT_EQ(agentId, agentAdded->agent().agent_info().id());

  AWAIT_READY(frameworkAdded);
  EXPECT_EQ(frameworkId, frameworkAdded->framework().framework_info().id());
  EXPECT_FALSE(frameworkAdded->framework().active());
  EXPECT_FALSE(frameworkAdded->framework().connected());
  EXPECT_TRUE(frameworkAdded->framework().recovered());

  AWAIT_READY(taskAdded);
  EXPECT_EQ(task.task_id(), taskAdded->task().task_id());
}


// Verifies that operators subscribed to the master's operator API event
// stream only receive events that they are authorized to see.
TEST_P(MasterAPITest, EventAuthorizationFiltering)
{
  ContentType contentType = GetParam();

  ACLs acls;

  // Only authorize the default credential to view tasks and frameworks of
  // the user 'root', thus task events related to other users will be invisible
  // to the default credential.

  {
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->add_values("root");
  }

  {
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Result<Authorizer*> authorizer = Authorizer::create(acls);

  Try<Owned<cluster::Master>> master = StartMaster(authorizer.get());

  ASSERT_SOME(master);

  ExecutorInfo executorInfo1;
  ExecutorID executorId1;
  {
    executorId1.set_value("executor_id_1");
    executorInfo1.set_name("executor_info_1");
    executorInfo1.mutable_executor_id()->CopyFrom(executorId1);
  }

  ExecutorInfo executorInfo2;
  ExecutorID executorId2;
  {
    executorId2.set_value("executor_id_2");
    executorInfo2.set_name("executor_info_2");
    executorInfo2.mutable_executor_id()->CopyFrom(executorId2);
  }

  MockExecutor executor1(executorId1);
  MockExecutor executor2(executorId2);

  hashmap<ExecutorID, Executor*> executors;
  executors[executorId1] = &executor1;
  executors[executorId2] = &executor2;

  TestContainerizer containerizer(executors);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We don't need to actually launch tasks as the specified user,
  // since we are only interested in testing the authorization path.
  slave::Flags slaveFlags = MesosTest::CreateSlaveFlags();

#ifndef __WINDOWS__
  slaveFlags.switch_user = false;
#endif

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_user("root");
  mesos.send(v1::createCallSubscribe(frameworkInfo));

  AWAIT_READY(subscribed);

  // Launch a task using the scheduler. This should result in a `TASK_ADDED`
  // event when the task is launched followed by a `TASK_UPDATED` event after
  // the task transitions to running state.
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  v1::AgentID agentId(offers1->offers()[0].agent_id());

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SUBSCRIBE);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();

  auto deserializer =
    lambda::bind(deserialize<v1::master::Event>, contentType, lambda::_1);

  Reader<v1::master::Event> decoder(deserializer, reader);

  {
    Future<Result<v1::master::Event>> event = decoder.read();
    AWAIT_READY(event);

    ASSERT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
    const v1::master::Response::GetState& getState =
        event->get().subscribed().get_state();

    EXPECT_EQ(1, getState.get_frameworks().frameworks_size());
    EXPECT_EQ(1, getState.get_agents().agents_size());
    EXPECT_EQ(0, getState.get_tasks().tasks_size());
    EXPECT_EQ(0, getState.get_executors().executors_size());
  }

  {
    Future<Result<v1::master::Event>> event = decoder.read();

    AWAIT_READY(event);

    EXPECT_EQ(v1::master::Event::HEARTBEAT, event->get().type());
  }

  Future<Result<v1::master::Event>> event = decoder.read();
  EXPECT_TRUE(event.isPending());

  Future<mesos::v1::scheduler::Event::Update> updateRunning1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&updateRunning1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(executor1, registered(_, _, _, _));
  EXPECT_CALL(executor2, registered(_, _, _, _));

  Future<TaskInfo> execTask1;
  EXPECT_CALL(executor1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&execTask1)));

  const v1::Offer& offer1 = offers1->offers(0);

  v1::TaskInfo task1;
  {
    task1.set_name("task1");
    task1.mutable_task_id()->set_value("1");
    task1.mutable_agent_id()->CopyFrom(offer1.agent_id());
    task1.mutable_resources()->CopyFrom(
        v1::Resources::parse("cpus:0.1;mem:32;disk:32").get());
    task1.mutable_executor()->CopyFrom(evolve(executorInfo1));
    task1.mutable_executor()->mutable_command()->set_value("sleep 1000");
    task1.mutable_executor()->mutable_command()->set_user("root");
  }

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::ACCEPT);

    call.mutable_framework_id()->CopyFrom(frameworkId);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer1.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);

    operation->mutable_launch()->add_task_infos()->CopyFrom(task1);

    mesos.send(call);
  }

  AWAIT_READY(event);

  ASSERT_EQ(v1::master::Event::TASK_ADDED, event->get().type());
  ASSERT_EQ(task1.task_id(), event->get().task_added().task().task_id());

  AWAIT_READY(updateRunning1);
  EXPECT_EQ(updateRunning1->status().state(), v1::TASK_RUNNING);

  event = decoder.read();

  AWAIT_READY(event);

  ASSERT_EQ(v1::master::Event::TASK_UPDATED, event->get().type());
  ASSERT_EQ(v1::TASK_RUNNING,
            event->get().task_updated().state());
  ASSERT_EQ(v1::TASK_RUNNING,
            event->get().task_updated().status().state());
  ASSERT_EQ(task1.task_id(),
            event->get().task_updated().status().task_id());

  // Summon an offer.
  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore further offers.

  {
    v1::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(v1::scheduler::Call::REVIVE);

    mesos.send(call);
  }

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  Future<mesos::v1::scheduler::Event::Update> updateRunning2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<TaskInfo> execTask2;
  EXPECT_CALL(executor2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&execTask2)));

  v1::TaskInfo task2;
  {
    task2.set_name("task2");
    task2.mutable_task_id()->set_value("2");
    task2.mutable_agent_id()->CopyFrom(offer2.agent_id());
    task2.mutable_resources()->CopyFrom(
        v1::Resources::parse("cpus:0.1;mem:32;disk:32").get());
    task2.mutable_executor()->CopyFrom(evolve(executorInfo2));
    task2.mutable_executor()->mutable_command()->set_value("sleep 1000");
    task2.mutable_executor()->mutable_command()->set_user("foo");
  }

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::ACCEPT);

    call.mutable_framework_id()->CopyFrom(frameworkId);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer2.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);

    operation->mutable_launch()->add_task_infos()->CopyFrom(task2);

    mesos.send(call);
  }

  AWAIT_READY(updateRunning2);

  event = decoder.read();
  EXPECT_TRUE(event.isPending());

  // To ensure that the TASK_ADDED event for task2 was filtered correctly,
  // we wait for the next heartbeat event.
  Clock::pause();
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);
  Clock::resume();

  AWAIT_READY(event);
  EXPECT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  EXPECT_TRUE(reader.close());

  EXPECT_CALL(executor1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(executor2, shutdown(_))
    .Times(AtMost(1));
}


// This test tries to verify that a client subscribed to the 'api/v1' endpoint
// can receive `FRAMEWORK_ADDED`, `FRAMEWORK_UPDATED` and 'FRAMEWORK_REMOVED'
// events.
TEST_P(MasterAPITest, FrameworksEvent)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SUBSCRIBE);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();

  auto deserializer =
    lambda::bind(deserialize<v1::master::Event>, contentType, lambda::_1);

  Reader<v1::master::Event> decoder(deserializer, reader);

  Future<Result<v1::master::Event>> event = decoder.read();
  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  const v1::master::Response::GetState& getState =
      event->get().subscribed().get_state();

  EXPECT_TRUE(getState.get_frameworks().frameworks().empty());
  EXPECT_TRUE(getState.get_agents().agents().empty());
  EXPECT_TRUE(getState.get_tasks().tasks().empty());
  EXPECT_TRUE(getState.get_executors().executors().empty());

  event = decoder.read();

  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  event = decoder.read();
  EXPECT_TRUE(event.isPending());

  // Start a scheduler. The subscriber will receive a 'FRAMEWORK_ADDED' event
  // when the scheduler subscribes with the master.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler,
      detector);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  // Set the timeout to a large value to avoid the framework being removed
  // when it reconnects.
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // We need to set `checkpoint` explicitly. Otherwise, `jsonify` in the
  // scheduler driver sets it to the default value, which results in a spurious
  // diff between the reported `FrameworkInfo` (`checkpoint == false`) and the
  // one with which the scheduler has subscribed (`checkpoint` not set).
  frameworkInfo.set_checkpoint(false);

  mesos.send(v1::createCallSubscribe(frameworkInfo));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId = subscribed->framework_id();
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  AWAIT_READY(event);

  ::mesos::v1::TimeInfo registeredTime;
  {
    EXPECT_EQ(v1::master::Event::FRAMEWORK_ADDED, event.get()->type());

    const v1::master::Response::GetFrameworks::Framework& framework =
      event.get()->framework_added().framework();

    EXPECT_NONE(
        mesos::v1::typeutils::diff(frameworkInfo, framework.framework_info()));

    EXPECT_TRUE(framework.active());
    EXPECT_TRUE(framework.connected());

    registeredTime = framework.registered_time();
  }

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  // Force a reconnection with the master. This should result in a
  // 'FRAMEWORK_UPDATED' event when the scheduler reregisters with the master.
  mesos.reconnect();

  AWAIT_READY(disconnected);

  // The scheduler should be able to immediately reconnect with the master.
  AWAIT_READY(connected);

  mesos.send(v1::createCallSubscribe(frameworkInfo, frameworkId));

  event = decoder.read();
  AWAIT_READY(event);

  {
    EXPECT_EQ(v1::master::Event::FRAMEWORK_UPDATED, event.get()->type());

    const v1::master::Response::GetFrameworks::Framework& framework =
      event.get()->framework_updated().framework();

    EXPECT_NONE(
        mesos::v1::typeutils::diff(frameworkInfo, framework.framework_info()));

    EXPECT_EQ(registeredTime, framework.registered_time());

    EXPECT_LT(
        registeredTime.nanoseconds(),
        framework.reregistered_time().nanoseconds());
  }

  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  // Send a teardown request to the master to teardown the framework.
  // The subscriber will receive a 'FRAMEWORK_REMOVED' event from the master.
  {
    Future<http::Response> response = process::http::post(
        master.get()->pid,
        "teardown",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        "frameworkId=" + frameworkId.value());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(disconnected);

  event = decoder.read();
  AWAIT_READY(event);

  {
    EXPECT_EQ(v1::master::Event::FRAMEWORK_REMOVED, event.get()->type());

    const v1::FrameworkID& frameworkId_ =
      event.get()->framework_removed().framework_info().id();

    EXPECT_EQ(frameworkId, frameworkId_);
  }
}


// Verifies that 'HEARTBEAT' events are sent at the correct times.
TEST_P(MasterAPITest, Heartbeat)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SUBSCRIBE);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();

  auto deserializer =
    lambda::bind(deserialize<v1::master::Event>, contentType, lambda::_1);

  Reader<v1::master::Event> decoder(deserializer, reader);

  Future<Result<v1::master::Event>> event = decoder.read();
  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  ASSERT_TRUE(event->get().subscribed().has_heartbeat_interval_seconds());
  EXPECT_EQ(
      DEFAULT_HEARTBEAT_INTERVAL.secs(),
      event->get().subscribed().heartbeat_interval_seconds());

  const v1::master::Response::GetState& getState =
      event->get().subscribed().get_state();

  EXPECT_EQ(0, getState.get_frameworks().frameworks_size());
  EXPECT_EQ(0, getState.get_agents().agents_size());
  EXPECT_EQ(0, getState.get_tasks().tasks_size());
  EXPECT_EQ(0, getState.get_executors().executors_size());

  event = decoder.read();

  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  event = decoder.read();
  EXPECT_TRUE(event.isPending());

  Clock::pause();

  // Expects a heartbeat event after every heartbeat interval.
  for (int i = 0; i < 10; i++) {
    // Advance the clock to receive another heartbeat.
    Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);

    AWAIT_READY(event);
    EXPECT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

    event = decoder.read();
    EXPECT_TRUE(event.isPending());
  }

  Clock::resume();
}


// Verifies that old subscribers are disconnected when too many
// active subscribers are attached to the master's event stream at once.
TEST_P(MasterAPITest, MaxEventStreamSubscribers)
{
  Clock::pause();

  ContentType contentType = GetParam();
  const string METRIC_NAME("master/operator_event_stream_subscribers");

  // Lower the max number of connections for this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.max_operator_event_stream_subscribers = 2;

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Define some objects we'll use for all the SUBSCRIBE calls.
  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SUBSCRIBE);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  auto deserializer =
    lambda::bind(deserialize<v1::master::Event>, contentType, lambda::_1);

  Future<Result<v1::master::Event>> event;

  // Send two connections to fill up the circular buffer.
  Future<http::Response> response1 = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response1);
  ASSERT_EQ(http::Response::PIPE, response1->type);
  ASSERT_SOME(response1->reader);
  http::Pipe::Reader reader1 = response1->reader.get();

  Reader<v1::master::Event> decoder1(deserializer, reader1);

  event = decoder1.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  event = decoder1.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  Future<http::Response> response2 = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response2);
  ASSERT_EQ(http::Response::PIPE, response2->type);
  ASSERT_SOME(response2->reader);
  http::Pipe::Reader reader2 = response2->reader.get();

  Reader<v1::master::Event> decoder2(deserializer, reader2);

  event = decoder2.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  event = decoder2.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  // Check the relevant metric for total active subscribers.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(2, metrics.values.at(METRIC_NAME));

  // Start a third connection.
  {
    // This is basically `http::streaming::post` unwrapped inside the
    // test body. We must do this in order to control the lifetime of
    // the `http::Connection`. The HTTP helper will keep the streaming
    // connection alive until the server closes it, but this test wants
    // to prematurely close the connection.
    http::URL url(
        "http",
        master.get()->pid.address.ip,
        master.get()->pid.address.port,
        strings::join("/", master.get()->pid.id, "api/v1"));

    http::Request request;
    request.method = "POST";
    request.url = url;
    request.keepAlive = false;
    request.headers = headers;
    request.body = serialize(contentType, v1Call);
    request.headers["Content-Type"] = stringify(contentType);

    Future<http::Connection> connection = http::connect(request.url);

    Future<http::Response> response3 = connection
      .then([request](http::Connection connection) {
        return connection.send(request, true);
      });

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response3);
    ASSERT_EQ(http::Response::PIPE, response3->type);
    ASSERT_SOME(response3->reader);
    http::Pipe::Reader reader3 = response3->reader.get();

    Reader<v1::master::Event> decoder3(deserializer, reader3);

    event = decoder3.read();
    AWAIT_READY(event);
    ASSERT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
    event = decoder3.read();
    AWAIT_READY(event);
    ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

    // The first connection should have been kicked out by the third.
    event = decoder1.read();
    AWAIT_READY(event);
    ASSERT_TRUE(event->isNone());

    // The connection will go out of scope and be destructed, which brings
    // the total active connections below the maximum.
  }

  // Verify that the second connection is still open.
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);
  event = decoder2.read();
  AWAIT_READY(event);
  ASSERT_TRUE(event->isSome());
  ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  // Check the metric again.
  metrics = Metrics();
  EXPECT_EQ(1, metrics.values.at(METRIC_NAME));

  // Start a fourth connection. This should be under the maximum
  // and should not cause any disconnections.
  Future<http::Response> response4 = http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response4);
  ASSERT_EQ(http::Response::PIPE, response4->type);
  ASSERT_SOME(response4->reader);
  http::Pipe::Reader reader4 = response4->reader.get();

  Reader<v1::master::Event> decoder4(deserializer, reader4);

  event = decoder4.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  event = decoder4.read();
  AWAIT_READY(event);
  ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  // Verify that the second connection is still open.
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);
  event = decoder2.read();
  AWAIT_READY(event);
  ASSERT_TRUE(event->isSome());
  ASSERT_EQ(v1::master::Event::HEARTBEAT, event->get().type());

  Clock::resume();
}


// This test verifies if we can retrieve the current quota status through
// `GET_QUOTA` call, after we set quota resources through `SET_QUOTA` call.
TEST_P(MasterAPITest, GetQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::Resources quotaResources =
    v1::Resources::parse("cpus:1;mem:512").get();

  ContentType contentType = GetParam();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::SET_QUOTA);

    v1::quota::QuotaRequest* quotaRequest =
      v1Call.mutable_set_quota()->mutable_quota_request();

    // Use the force flag for setting quota that cannot be satisfied in
    // this empty cluster without any agents.
    quotaRequest->set_force(true);
    quotaRequest->set_role("role1");
    quotaRequest->mutable_guarantee()->CopyFrom(quotaResources);

    // Send a quota request for the specified role.
    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Verify the quota is set using the `GET_QUOTA` call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_QUOTA);

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_QUOTA, v1Response->type());
    ASSERT_EQ(1, v1Response->get_quota().status().infos().size());
    EXPECT_EQ(quotaResources,
              v1Response->get_quota().status().infos(0).guarantee());
  }
}


TEST_P(MasterAPITest, SetQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::Resources quotaResources =
    v1::Resources::parse("cpus:1;mem:512").get();

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SET_QUOTA);

  v1::quota::QuotaRequest* quotaRequest =
    v1Call.mutable_set_quota()->mutable_quota_request();

  // Use the force flag for setting quota that cannot be satisfied in
  // this empty cluster without any agents.
  quotaRequest->set_force(true);
  quotaRequest->set_role("role1");
  quotaRequest->mutable_guarantee()->CopyFrom(quotaResources);

  ContentType contentType = GetParam();

  // Send a quota request for the specified role.
  Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
}


// This test verifies if we can remove a quota through `REMOVE_QUOTA` call,
// after we set quota resources through `SET_QUOTA` call.
TEST_P(MasterAPITest, RemoveQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::Resources quotaResources =
    v1::Resources::parse("cpus:1;mem:512").get();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::SET_QUOTA);

    v1::quota::QuotaRequest* quotaRequest =
      v1Call.mutable_set_quota()->mutable_quota_request();

    // Use the force flag for setting quota that cannot be satisfied in
    // this empty cluster without any agents.
    quotaRequest->set_force(true);
    quotaRequest->set_role("role1");
    quotaRequest->mutable_guarantee()->CopyFrom(quotaResources);

    ContentType contentType = GetParam();

    // Send a quota request for the specified role.
    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Verify if the quota is set using `GET_QUOTA` call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_QUOTA);

    ContentType contentType = GetParam();

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_QUOTA, v1Response->type());
    ASSERT_EQ(1, v1Response->get_quota().status().infos().size());
    EXPECT_EQ(quotaResources,
      v1Response->get_quota().status().infos(0).guarantee());
  }

  // Remove the quota using `REMOVE_QUOTA` call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::REMOVE_QUOTA);
    v1Call.mutable_remove_quota()->set_role("role1");

    ContentType contentType = GetParam();

    // Send a quota request for the specified role.
    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Verify if the quota is removed using `GET_QUOTA` call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_QUOTA);

    ContentType contentType = GetParam();

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_QUOTA, v1Response->type());
    ASSERT_TRUE(v1Response->get_quota().status().infos().empty());
  }
}


// Test create and destroy persistent volumes through the master operator API.
// In this test case, we create a persistent volume with the API, then launch a
// task using the volume. Then we destroy the volume with the API after the task
// is finished.
TEST_P(MasterAPITest, CreateAndDestroyVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID so we can use it in the create/destroy volumes
  // API call.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  // Do Static reservation so we can create persistent volumes from it.
  slaveFlags.resources = "disk(role1):1024";

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Create the persistent volume.
  v1::master::Call v1CreateVolumesCall;
  v1CreateVolumesCall.set_type(v1::master::Call::CREATE_VOLUMES);
  v1::master::Call_CreateVolumes* createVolumes =
    v1CreateVolumesCall.mutable_create_volumes();

  Resource volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  createVolumes->add_volumes()->CopyFrom(evolve(volume));
  createVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));

  ContentType contentType = GetParam();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> v1CreateVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1CreateVolumesResponse);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  // Start a framework and launch a task on the persistent volume.
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
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.roles(0))));

  Resources taskResources = Resources::parse(
      "disk:256",
      frameworkInfo.roles(0)).get();

  TaskInfo taskInfo = createTask(
      offer.slave_id(),
      taskResources,
      "sleep 1",
      DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_FINISHED, status->state());

  // Destroy the persistent volume.
  v1::master::Call v1DestroyVolumesCall;
  v1DestroyVolumesCall.set_type(v1::master::Call::DESTROY_VOLUMES);
  v1::master::Call_DestroyVolumes* destroyVolumes =
    v1DestroyVolumesCall.mutable_destroy_volumes();

  destroyVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));
  destroyVolumes->add_volumes()->CopyFrom(evolve(volume));

  Future<http::Response> v1DestroyVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1DestroyVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1DestroyVolumesResponse);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Test growing a persistent volume through the master operator API.
TEST_P(MasterAPITest, GrowVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID so we can use it in API calls.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  // Do Static reservation so we can create persistent volumes from it.
  slaveFlags.resources = "disk(role1):192";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Create the persistent volume.
  v1::master::Call v1CreateVolumesCall;
  v1CreateVolumesCall.set_type(v1::master::Call::CREATE_VOLUMES);
  v1::master::Call_CreateVolumes* createVolumes =
    v1CreateVolumesCall.mutable_create_volumes();

  Resource volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  createVolumes->add_volumes()->CopyFrom(evolve(volume));
  createVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));

  ContentType contentType = GetParam();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> v1CreateVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1CreateVolumesResponse);

  Resource addition = Resources::parse("disk", "128", "role1").get();

  // Grow the persistent volume.
  v1::master::Call v1GrowVolumeCall;
  v1GrowVolumeCall.set_type(v1::master::Call::GROW_VOLUME);

  v1::master::Call::GrowVolume* growVolume =
    v1GrowVolumeCall.mutable_grow_volume();

  growVolume->mutable_agent_id()->CopyFrom(evolve(slaveId));
  growVolume->mutable_volume()->CopyFrom(evolve(volume));
  growVolume->mutable_addition()->CopyFrom(evolve(addition));

  Future<http::Response> v1GrowVolumeResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1GrowVolumeCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1GrowVolumeResponse);

  Resource grownVolume = createPersistentVolume(
      Megabytes(192),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  v1::master::Call v1GetAgentsCall;
  v1GetAgentsCall.set_type(v1::master::Call::GET_AGENTS);

  Future<v1::master::Response> v1GetAgentsResponse =
    post(master.get()->pid, v1GetAgentsCall, contentType);

  AWAIT_READY(v1GetAgentsResponse);
  ASSERT_TRUE(v1GetAgentsResponse->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_AGENTS, v1GetAgentsResponse->type());
  ASSERT_EQ(1, v1GetAgentsResponse->get_agents().agents_size());

  RepeatedPtrField<Resource> agentResources = devolve<Resource>(
      v1GetAgentsResponse->get_agents().agents(0).total_resources());

  upgradeResources(&agentResources);

  EXPECT_EQ(
      Resources(grownVolume),
      Resources(agentResources).persistentVolumes());
}


// Test shrinking a persistent volume through the master operator API.
TEST_P(MasterAPITest, ShrinkVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID so we can use it in API calls.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  // Do static reservation so we can create persistent volumes from it.
  slaveFlags.resources = "disk(role1):192";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Create a persistent volume with all disk space.
  v1::master::Call v1CreateVolumesCall;
  v1CreateVolumesCall.set_type(v1::master::Call::CREATE_VOLUMES);
  v1::master::Call_CreateVolumes* createVolumes =
    v1CreateVolumesCall.mutable_create_volumes();

  Resource volume = createPersistentVolume(
      Megabytes(192),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  createVolumes->add_volumes()->CopyFrom(evolve(volume));
  createVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));

  ContentType contentType = GetParam();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> v1CreateVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1CreateVolumesResponse);

  Resource shrunkVolume = createPersistentVolume(
      Megabytes(128),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  // Shrink the persistent volume.
  v1::master::Call v1ShrinkVolumeCall;
  v1ShrinkVolumeCall.set_type(v1::master::Call::SHRINK_VOLUME);

  v1::master::Call::ShrinkVolume* shrinkVolume =
    v1ShrinkVolumeCall.mutable_shrink_volume();

  shrinkVolume->mutable_agent_id()->CopyFrom(evolve(slaveId));
  shrinkVolume->mutable_volume()->CopyFrom(evolve(volume));
  shrinkVolume->mutable_subtract()->set_value(64);

  Future<http::Response> v1ShrinkVolumeResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1ShrinkVolumeCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Accepted().status,
      v1ShrinkVolumeResponse);

  v1::master::Call v1GetAgentsCall;
  v1GetAgentsCall.set_type(v1::master::Call::GET_AGENTS);

  Future<v1::master::Response> v1GetAgentsResponse =
    post(master.get()->pid, v1GetAgentsCall, contentType);

  AWAIT_READY(v1GetAgentsResponse);
  ASSERT_TRUE(v1GetAgentsResponse->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_AGENTS, v1GetAgentsResponse->type());
  ASSERT_EQ(1, v1GetAgentsResponse->get_agents().agents_size());

  RepeatedPtrField<Resource> agentResources = devolve<Resource>(
      v1GetAgentsResponse->get_agents().agents(0).total_resources());

  upgradeResources(&agentResources);

  EXPECT_EQ(
      Resources(shrunkVolume),
      Resources(agentResources).persistentVolumes());
}


TEST_P(MasterAPITest, GetWeights)
{
  // Start a master with `--weights` flag.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.weights = "role=2.0";
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_WEIGHTS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_WEIGHTS, v1Response->type());
  ASSERT_EQ(1, v1Response->get_weights().weight_infos_size());
  ASSERT_EQ("role", v1Response->get_weights().weight_infos().Get(0).role());
  ASSERT_EQ(2.0, v1Response->get_weights().weight_infos().Get(0).weight());
}


TEST_P(MasterAPITest, UpdateWeights)
{
  // Start a master with `--weights` flag.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.weights = "role=2.0";
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  v1::master::Call getCall, updateCall;
  getCall.set_type(v1::master::Call::GET_WEIGHTS);
  updateCall.set_type(v1::master::Call::UPDATE_WEIGHTS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> getResponse =
    post(master.get()->pid, getCall, contentType);

  AWAIT_READY(getResponse);
  ASSERT_TRUE(getResponse->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_WEIGHTS, getResponse->type());
  ASSERT_EQ(1, getResponse->get_weights().weight_infos_size());
  ASSERT_EQ("role", getResponse->get_weights().weight_infos().Get(0).role());
  ASSERT_EQ(2.0, getResponse->get_weights().weight_infos().Get(0).weight());

  v1::WeightInfo* weightInfo =
    updateCall.mutable_update_weights()->add_weight_infos();
  weightInfo->set_role("role");
  weightInfo->set_weight(4.0);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> updateResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, updateCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, updateResponse);

  getResponse = post(master.get()->pid, getCall, contentType);

  AWAIT_READY(getResponse);
  ASSERT_TRUE(getResponse->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_WEIGHTS, getResponse->type());
  ASSERT_EQ(1, getResponse->get_weights().weight_infos_size());
  ASSERT_EQ("role", getResponse->get_weights().weight_infos().Get(0).role());
  ASSERT_EQ(4.0, getResponse->get_weights().weight_infos().Get(0).weight());
}


// This test verifies if we can retrieve file data in the master.
TEST_P(MasterAPITest, ReadFile)
{
  Files files;

  // Now write a file.
  ASSERT_SOME(os::write("file", "body"));
  AWAIT_EXPECT_READY(files.attach("file", "myname"));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  ContentType contentType = GetParam();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::READ_FILE);

    v1::master::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(1);
    readFile->set_length(2);
    readFile->set_path("myname");

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("od", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file with `offset >= size`. This should return the size of file
  // and empty data.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::READ_FILE);

    v1::master::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(5);
    readFile->set_length(2);
    readFile->set_path("myname");

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file without length being set and `offset=0`. This should read
  // the entire file.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::READ_FILE);

    v1::master::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(0);
    readFile->set_path("myname");

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("body", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file with `length > size - offset`. This should return the
  // data actually read.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::READ_FILE);

    v1::master::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(1);
    readFile->set_length(6);
    readFile->set_path("myname");

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("ody", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }
}


// This test verifies that the client will receive a `NotFound` response when
// it tries to make a `READ_FILE` call with an invalid path.
TEST_P(MasterAPITest, ReadFileInvalidPath)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Read an invalid file.
  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::READ_FILE);

  v1::master::Call::ReadFile* readFile = v1Call.mutable_read_file();
  readFile->set_offset(1);
  readFile->set_length(2);
  readFile->set_path("invalid_file");

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
}


// This test verifies that when the operator API TEARDOWN call is made,
// the framework is shutdown and removed. It also confirms that authorization
// of this call is performed correctly.
TEST_P(MasterAPITest, Teardown)
{
  ContentType contentType = GetParam();

  ACLs acls;

  // Only allow DEFAULT_CREDENTIAL to teardown frameworks.
  {
    mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_framework_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_framework_principals()->add_values(
        DEFAULT_CREDENTIAL.principal());
  }

  master::Flags masterFlags = CreateMasterFlags();
  Result<Authorizer*> authorizer = Authorizer::create(acls);

  Try<Owned<cluster::Master>> master =
    StartMaster(authorizer.get(), masterFlags);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  AWAIT_READY(registerSlaveMessage);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore subsequent connections.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(Return()); // Ignore offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_user("root");
  mesos.send(v1::createCallSubscribe(frameworkInfo));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId = subscribed->framework_id();

  // There should be one framework in the response of the 'GET_FRAMEWORKS' call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_FRAMEWORKS);

    Future<v1::master::Response> v1Response =
        post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_FRAMEWORKS, v1Response->type());

    v1::master::Response::GetFrameworks frameworks =
        v1Response->get_frameworks();

    ASSERT_EQ(1, frameworks.frameworks_size());
  }

  // Send teardown with principal that is not authorized.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::TEARDOWN);

    v1::master::Call::Teardown* teardown = v1Call.mutable_teardown();

    teardown->mutable_framework_id()->CopyFrom(frameworkId);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  // There should still be one framework in the response.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_FRAMEWORKS);

    Future<v1::master::Response> v1Response =
        post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_FRAMEWORKS, v1Response->type());

    v1::master::Response::GetFrameworks frameworks =
        v1Response->get_frameworks();

    ASSERT_EQ(1, frameworks.frameworks_size());
  }

  Future<ShutdownFrameworkMessage> shutdownFrameworkMessage =
    FUTURE_PROTOBUF(ShutdownFrameworkMessage(), _, _);

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  // Send the teardown call with the correct credential, it will teardown the
  // framework.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::TEARDOWN);

    v1::master::Call::Teardown* teardown = v1Call.mutable_teardown();

    teardown->mutable_framework_id()->CopyFrom(frameworkId);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(shutdownFrameworkMessage);
  AWAIT_READY(disconnected);

  // There should be one framework in the 'completed_frameworks' field of
  // the response for the 'GET_FRAMEWORKS' call.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_FRAMEWORKS);

    Future<v1::master::Response> v1Response =
        post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_FRAMEWORKS, v1Response->type());

    v1::master::Response::GetFrameworks frameworks =
        v1Response->get_frameworks();

    ASSERT_EQ(0, frameworks.frameworks_size());
    ASSERT_EQ(1, frameworks.completed_frameworks_size());
  }

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// This test verifies that a registered agent can be marked as gone and
// shutdown by the master subsequently. Upon restarting the agent, it
// should not be able to reregister with the master.
TEST_P(MasterAPITest, MarkRegisteredAgentGone)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Ensure that the agent is registered successfully with the master
  // before marking it as gone.
  AWAIT_READY(slaveRegisteredMessage);

  // Mark the agent as gone. This should result in the agent being shutdown.

  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get()->pid, _);

  ContentType contentType = GetParam();

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

  v1::master::Call::MarkAgentGone* markAgentGone =
    v1Call.mutable_mark_agent_gone();

  markAgentGone->mutable_agent_id()->CopyFrom(
      evolve(slaveRegisteredMessage->slave_id()));

  Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_READY(shutdownMessage);

  // The agent should not be able to reregister with
  // the master upon restart.

  slave.get()->terminate();
  slave->reset();

  Future<ShutdownMessage> shutdownMessage2 =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get()->pid, _);

  slave = StartSlave(detector.get(), slaveFlags);

  AWAIT_READY(shutdownMessage2);
}


// This test verifies that unauthorized principals are unable to
// mark agents as gone.
TEST_P_TEMP_DISABLED_ON_WINDOWS(MasterAPITest,
                                MarkRegisteredAgentGoneUnauthorized)
{
  master::Flags masterFlags = CreateMasterFlags();

  {
    // Default principal is not allowed to mark agents as gone.
    mesos::ACL::MarkAgentGone* acl = masterFlags.acls->add_mark_agents_gone();

    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_agents()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Ensure that the agent is registered successfully with the master
  // before marking it as gone.
  AWAIT_READY(slaveRegisteredMessage);

  // Mark the agent as gone. This should fail due to an authorization error.

  ContentType contentType = GetParam();

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

  v1::master::Call::MarkAgentGone* markAgentGone =
    v1Call.mutable_mark_agent_gone();

  markAgentGone->mutable_agent_id()->CopyFrom(
      evolve(slaveRegisteredMessage->slave_id()));

  Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
}


// This test verifies that the master correctly sends 'TASK_GONE_BY_OPERATOR'
// status updates when an agent running the tasks is marked as gone.
TEST_P(MasterAPITest, TaskUpdatesUponAgentGone)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(update);
  ASSERT_EQ(TASK_RUNNING, update->state());

  Future<TaskStatus> update2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update2));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Mark the agent as gone. This should result in the master sending
  // a 'TASK_GONE_BY_OPERATOR' update for the running task.

  ContentType contentType = GetParam();

  SlaveID slaveId = offers.get()[0].slave_id();

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

  v1::master::Call::MarkAgentGone* markAgentGone =
    v1Call.mutable_mark_agent_gone();

  markAgentGone->mutable_agent_id()->CopyFrom(evolve(slaveId));

  Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

  AWAIT_READY(update2);

  ASSERT_EQ(TASK_GONE_BY_OPERATOR, update2->state());
  ASSERT_EQ(TaskStatus::REASON_SLAVE_REMOVED_BY_OPERATOR, update2->reason());

  // Performing reconciliation for an unknown task on the gone agent should
  // result in a 'TASK_GONE_BY_OPERATOR' update.

  Future<TaskStatus> update3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update3));

  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.mutable_task_id()->set_value("dummy-task");

  statuses.push_back(status);

  driver.reconcileTasks(statuses);

  AWAIT_READY(update3);
  ASSERT_EQ(TASK_GONE_BY_OPERATOR, update3->state());

  driver.stop();
  driver.join();
}


// This test verifies that the master correctly sends 'TASK_GONE_BY_OPERATOR'
// status updates and transitions unreachable tasks to completed when an
// unreachable agent which was running the tasks is marked as gone.
TEST_P(MasterAPITest, UnreachableAgentMarkedGone)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

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

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  auto mesos = std::make_shared<v1::scheduler::TestMesos>(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  // Launch a task on this agent so that agent removal will cause
  // the master to look for the framework struct.

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Try<v1::Resources> resources =
    v1::Resources::parse("cpus:0.1;mem:64;disk:64");

  ASSERT_SOME(resources);

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources.get(), SLEEP_COMMAND(1000));

  testing::Sequence updateSequence;
  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;

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
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return());

  mesos->send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);

  Future<v1::scheduler::Event::Update> unreachableUpdate;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_UNREACHABLE))))
    .WillOnce(FutureArg<1>(&unreachableUpdate));

  EXPECT_CALL(*scheduler, failure(_, _));

  // Detect pings from master to agent.
  Future<process::Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all PONGs to simulate agent partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  // Advance the clock to produce a ping.
  Clock::advance(masterFlags.agent_ping_timeout);

  // Now advance through enough pings to mark the agent unreachable.
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

  AWAIT_READY(unreachableUpdate);

  Future<v1::scheduler::Event::Update> goneUpdate;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
            TaskStatusUpdateTaskIdEq(taskInfo.task_id()),
            TaskStatusUpdateStateEq(v1::TASK_GONE_BY_OPERATOR))))
    .WillOnce(FutureArg<1>(&goneUpdate));

  ContentType contentType = GetParam();

  // Mark the agent as gone. This should result in the master sending
  // a 'TASK_GONE_BY_OPERATOR' update for the running task.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

    v1::master::Call::MarkAgentGone* markAgentGone =
      v1Call.mutable_mark_agent_gone();

    markAgentGone->mutable_agent_id()->CopyFrom(agentId);

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(goneUpdate);

  // GetState after agent is marked gone to ensure that the previously
  // unreachable task has been moved to completed.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_STATE);

    Future<v1::master::Response> v1Response =
      post(master.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    const v1::master::Response::GetState& getState = v1Response->get_state();
    ASSERT_TRUE(getState.get_tasks().unreachable_tasks().empty());
    ASSERT_EQ(1, getState.get_tasks().completed_tasks_size());
    ASSERT_EQ(
        taskInfo.task_id(),
        getState.get_tasks().completed_tasks(0).task_id());
  }

  Clock::resume();
}


// This test verifies that the master correctly sends
// `OPERATION_GONE_BY_OPERATOR` status updates for operations
// that are pending when the agent is marked as gone.
TEST_P(MasterAPITest, OperationUpdatesUponAgentGone)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);
  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.
  v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  Owned<v1::TestResourceProvider> resourceProvider(
      new v1::TestResourceProvider(resourceProviderInfo, v1::Resources(disk)));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider->start(endpointDetector, ContentType::PROTOBUF);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  // Make sure the allocator is updated to include disk resources from
  // the resource provider.
  Clock::settle();

  // Start and register a framework.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

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

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Don't let the message get to the agent, so it stays pending.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // Try to reserve those resources managed by a resource provider,
  // because operation feedback is only supported for that case.
  v1::Resources resources =
    v1::Resources(offer.resources()).filter([](const v1::Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  v1::Resource reserved = *(resources.begin());
  reserved.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  // Explicitly set an operation id to opt-in to operation feedback.
  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::RESERVE(reserved, operationId)}));

  AWAIT_READY(applyOperationMessage);

  // Mark the agent as gone. This should result in the agent being shutdown.
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get()->pid, _);

  // Mark the agent as gone. This should result in the master sending
  // a 'TASK_GONE_BY_OPERATOR' update for the running task.
  Future<v1::scheduler::Event::UpdateOperationStatus> operationGoneUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&operationGoneUpdate));

  ContentType contentType = GetParam();

  v1::AgentID agentId = offers->offers(0).agent_id();

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

  v1::master::Call::MarkAgentGone* markAgentGone =
    v1Call.mutable_mark_agent_gone();

  markAgentGone->mutable_agent_id()->CopyFrom(agentId);

  Future<http::Response> response = http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

  AWAIT_READY(shutdownMessage);

  // Wait for the framework to receive the OPERATION_GONE_BY_OPERATOR update.
  AWAIT_READY(operationGoneUpdate);

  EXPECT_EQ(operationId, operationGoneUpdate->status().operation_id());
  EXPECT_EQ(
      v1::OPERATION_GONE_BY_OPERATOR,
      operationGoneUpdate->status().state());

  // TODO(bevers): We have to reset the agent before we can access the
  // metrics to work around MESOS-9644.
  slave->reset();
  EXPECT_TRUE(metricEquals("master/operations/gone_by_operator", 1));
}


// This test verifies that the master correctly sends
// `OPERATION_UNREACHABLE` status updates for operations
// that are pending when the agent is marked as gone.
TEST_P(MasterAPITest, OperationUpdatesUponUnreachable)
{
  Clock::pause();

  // Start master and agent.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  slave::Flags slaveFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  // Start and register a resource provider.
  v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  Owned<v1::TestResourceProvider> resourceProvider(
      new v1::TestResourceProvider(resourceProviderInfo, v1::Resources(disk)));

  Owned<EndpointDetector> endpointDetector(
      mesos::internal::tests::resource_provider::createEndpointDetector(
          slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider->start(endpointDetector, ContentType::PROTOBUF);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a framework.
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

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Don't let the message get to the agent, so the operation stays pending.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // Try to reserve those resources managed by a resource provider.
  // This is technically not necessary, but support for reserving
  // resources not managed by a resource provider was only added
  // after this test was written.
  v1::Resources resources =
    v1::Resources(offer.resources()).filter([](const v1::Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  v1::Resource reserved = *(resources.begin());
  reserved.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  // Explicitly set an operation id to opt-in to operation feedback.
  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::RESERVE(reserved, operationId)}));

  AWAIT_READY(applyOperationMessage);

  using UpdateOperationStatus = v1::scheduler::Event::UpdateOperationStatus;
  Future<UpdateOperationStatus> operationUnreachableUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&operationUnreachableUpdate));

  // Terminate the agent abruptly. This causes the master -> agent
  // socket to break on the master side.
  slave.get()->terminate();
  Clock::settle();
  Clock::advance(masterFlags.agent_reregister_timeout);

  // Wait for the framework to receive the OPERATION_UNREACHABLE update.
  AWAIT_READY(operationUnreachableUpdate);

  EXPECT_EQ(operationId, operationUnreachableUpdate->status().operation_id());
  EXPECT_EQ(
      v1::OPERATION_UNREACHABLE,
      operationUnreachableUpdate->status().state());

  // TODO(bevers): We have to reset the agent before we can access the
  // metrics to work around MESOS-9644.
  slave->reset();
  EXPECT_TRUE(metricEquals("master/operations/unreachable", 1));

  Clock::resume();
}


class AgentAPITest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  master::Flags CreateMasterFlags() override
  {
    // Turn off periodic allocations to avoid the race between
    // `HierarchicalAllocator::updateAvailable()` and periodic allocations.
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Seconds(1000);
    return flags;
  }

  // Helper function to post a request to "/api/v1" agent endpoint and return
  // the response.
  Future<v1::agent::Response> post(
      const process::PID<slave::Slave>& pid,
      const v1::agent::Call& call,
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
        stringify(contentType))
      .then([contentType](const http::Response& response)
            -> Future<v1::agent::Response> {
        if (response.status != http::OK().status) {
          return Failure("Unexpected response status " + response.status);
        }
        return deserialize<v1::agent::Response>(contentType, response.body);
      });
  }
};


// These tests are parameterized by the content type of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    AgentAPITest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// Reads `ProcessIO::Data` records from the pipe `reader` until EOF is reached
// and returns the merged stdout and stderr.
// NOTE: It ignores any `ProcessIO::Control` records.
static Future<tuple<string, string>> getProcessIOData(
    ContentType contentType,
    http::Pipe::Reader reader)
{
  return reader.readAll()
    .then([contentType](const string& data) -> Future<tuple<string, string>> {
      string stdoutReceived;
      string stderrReceived;

      ::recordio::Decoder decoder;

      Try<std::deque<string>> records = decoder.decode(data);

      if (records.isError()) {
        return process::Failure(records.error());
      }

      while(!records->empty()) {
        string record = std::move(records->front());
        records->pop_front();

        Try<v1::agent::ProcessIO> processIO =
          deserialize<v1::agent::ProcessIO>(contentType, record);

        if (processIO.isError()) {
          return process::Failure(processIO.error());
        }

        if (processIO->data().type() == v1::agent::ProcessIO::Data::STDOUT) {
          stdoutReceived += processIO->data().data();
        } else if (processIO->data().type() ==
            v1::agent::ProcessIO::Data::STDERR) {
          stderrReceived += processIO->data().data();
        }
      }

      return std::make_tuple(stdoutReceived, stderrReceived);
    });
}


TEST_P(AgentAPITest, GetFlags)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_FLAGS);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_FLAGS, v1Response->type());
}


TEST_P(AgentAPITest, GetHealth)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_HEALTH);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_HEALTH, v1Response->type());
  ASSERT_TRUE(v1Response->get_health().healthy());
}


TEST_P(AgentAPITest, GetVersion)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_VERSION);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_VERSION, v1Response->type());

  ASSERT_EQ(MESOS_VERSION,
            v1Response->get_version().version_info().version());
}


TEST_P(AgentAPITest, GetMetrics)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  Duration timeout = Seconds(5);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_METRICS);
  v1Call.mutable_get_metrics()->mutable_timeout()->set_nanoseconds(
      timeout.ns());

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_METRICS, v1Response->type());

  hashmap<string, double> metrics;

  foreach (const v1::Metric& metric,
           v1Response->get_metrics().metrics()) {
    ASSERT_TRUE(metric.has_value());
    metrics[metric.name()] = metric.value();
  }

  // Verifies that the response metrics is not empty.
  ASSERT_LE(0u, metrics.size());
}


TEST_P(AgentAPITest, GetLoggingLevel)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_LOGGING_LEVEL);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_LOGGING_LEVEL, v1Response->type());
  ASSERT_LE(0, FLAGS_v);
  ASSERT_EQ(
      v1Response->get_logging_level().level(),
      static_cast<uint32_t>(FLAGS_v));
}


// Test the logging level toggle and revert after specific toggle duration.
TEST_P(AgentAPITest, SetLoggingLevel)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  // We capture the original logging level first; it would be used to verify
  // the logging level revert works.
  uint32_t originalLevel = static_cast<uint32_t>(FLAGS_v);

  // Send request to agent to toggle the logging level.
  uint32_t toggleLevel = originalLevel + 1;
  Duration toggleDuration = Seconds(60);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::SET_LOGGING_LEVEL);
  v1::agent::Call_SetLoggingLevel* setLoggingLevel =
    v1Call.mutable_set_logging_level();
  setLoggingLevel->set_level(toggleLevel);
  setLoggingLevel->mutable_duration()->set_nanoseconds(toggleDuration.ns());

  ContentType contentType = GetParam();
  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> v1Response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, v1Response);
  ASSERT_EQ(toggleLevel, static_cast<uint32_t>(FLAGS_v));

  // Speedup the logging level revert.
  Clock::pause();
  Clock::advance(toggleDuration);
  Clock::settle();

  // Verifies the logging level reverted successfully.
  ASSERT_EQ(originalLevel, static_cast<uint32_t>(FLAGS_v));
  Clock::resume();
}


// This test verifies if we can retrieve the file listing for a directory
// in an agent.
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, ListFiles)
{
  Files files;

  ASSERT_SOME(os::mkdir("1/2"));
  ASSERT_SOME(os::mkdir("1/3"));
  ASSERT_SOME(os::write("1/two", "two"));

  AWAIT_EXPECT_READY(files.attach("1", "one"));

  // Get the `FileInfo` for "1/two" file.
  struct stat s;
  ASSERT_EQ(0, stat("1/two", &s));
  FileInfo file = protobuf::createFileInfo("one/two", s);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::LIST_FILES);
  v1Call.mutable_list_files()->set_path("one/");

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::LIST_FILES, v1Response->type());
  ASSERT_EQ(3, v1Response->list_files().file_infos().size());
  ASSERT_EQ(evolve(file), v1Response->list_files().file_infos(2));
}


// This test verifies that the client will receive a `NotFound` response when it
// tries to make a `LIST_FILES` call with an invalid path.
TEST_P(AgentAPITest, ListFilesInvalidPath)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::LIST_FILES);
  v1Call.mutable_list_files()->set_path("five/");

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
    slave.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
}


TEST_P(AgentAPITest, GetContainers)
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

  EXPECT_CALL(sched, registered(_, _, _));
  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      "sleep 1000",
      exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  // No tasks launched, we should expect zero containers in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_CONTAINERS);

    ContentType contentType = GetParam();

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_CONTAINERS, v1Response->type());
    ASSERT_TRUE(v1Response->get_containers().containers().empty());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ResourceStatistics statistics;
  statistics.set_mem_limit_bytes(2048);
  // We have to set timestamp here since serializing protobuf without
  // filling all required fields generates errors.
  statistics.set_timestamp(0);

  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(statistics));

  ContainerStatus containerStatus;
  NetworkInfo* networkInfo = containerStatus.add_network_infos();
  NetworkInfo::IPAddress* ipAddr = networkInfo->add_ip_addresses();
  ipAddr->set_ip_address("192.168.1.20");

  EXPECT_CALL(containerizer, status(_))
    .WillRepeatedly(Return(containerStatus));

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_CONTAINERS);

  ContentType contentType = GetParam();
  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_CONTAINERS, v1Response->type());
  ASSERT_EQ(1, v1Response->get_containers().containers_size());
  ASSERT_EQ("192.168.1.20",
            v1Response->get_containers().containers(0).container_status()
              .network_infos(0).ip_addresses(0).ip_address());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies if we can retrieve file data in the agent.
TEST_P(AgentAPITest, ReadFile)
{
  Files files;

  // Now write a file.
  ASSERT_SOME(os::write("file", "body"));
  AWAIT_EXPECT_READY(files.attach("file", "myname"));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  ContentType contentType = GetParam();

  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::READ_FILE);

    v1::agent::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(1);
    readFile->set_length(2);
    readFile->set_path("myname");

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("od", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file with `offset >= size`. This should return the size of file
  // and empty data.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::READ_FILE);

    v1::agent::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(5);
    readFile->set_length(2);
    readFile->set_path("myname");

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file without length being set and `offset=0`. This should read
  // the entire file.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::READ_FILE);

    v1::agent::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(0);
    readFile->set_path("myname");

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("body", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }

  // Read the file with `length > size - offset`. This should return the
  // data actually read.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::READ_FILE);

    v1::agent::Call::ReadFile* readFile = v1Call.mutable_read_file();
    readFile->set_offset(1);
    readFile->set_length(6);
    readFile->set_path("myname");

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);

    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::READ_FILE, v1Response->type());

    ASSERT_EQ("ody", v1Response->read_file().data());
    ASSERT_EQ(4u, v1Response->read_file().size());
  }
}


// This test verifies that the client will receive a `NotFound` response when
// it tries to make a `READ_FILE` call with an invalid path.
TEST_P(AgentAPITest, ReadFileInvalidPath)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  // Read an invalid file.
  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::READ_FILE);

  v1::agent::Call::ReadFile* readFile = v1Call.mutable_read_file();
  readFile->set_offset(1);
  readFile->set_length(2);
  readFile->set_path("invalid_file");

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
    slave.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, GetFrameworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  ContentType contentType = GetParam();

  // No tasks launched, we should expect zero frameworks in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_FRAMEWORKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_FRAMEWORKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_frameworks().frameworks().empty());
    ASSERT_TRUE(v1Response->get_frameworks().completed_frameworks().empty());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // A task launched, we expect one framework in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_FRAMEWORKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_FRAMEWORKS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_frameworks().frameworks_size());
    ASSERT_TRUE(v1Response->get_frameworks().completed_frameworks().empty());
  }

  // Make sure the executor terminated.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  driver.stop();
  driver.join();

  AWAIT_READY(executorTerminated);

  // After the executor terminated, we should expect one completed framework in
  // Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_FRAMEWORKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_FRAMEWORKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_frameworks().frameworks().empty());
    ASSERT_EQ(1, v1Response->get_frameworks().completed_frameworks_size());
  }
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, GetExecutors)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  ContentType contentType = GetParam();

  // No tasks launched, we should expect zero executors in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_EXECUTORS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_EXECUTORS, v1Response->type());
    ASSERT_TRUE(v1Response->get_executors().executors().empty());
    ASSERT_TRUE(v1Response->get_executors().completed_executors().empty());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // A task launched, we expect one executor in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_EXECUTORS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_EXECUTORS, v1Response->type());
    ASSERT_EQ(1, v1Response->get_executors().executors_size());
    ASSERT_TRUE(v1Response->get_executors().completed_executors().empty());
  }

  // Make sure the executor terminated.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  driver.stop();
  driver.join();

  AWAIT_READY(executorTerminated);

  // Make sure `Framework::destroyExecutor()` is processed.
  Clock::pause();
  Clock::settle();

  // After the executor terminated, we should expect one completed executor in
  // Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_EXECUTORS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_EXECUTORS, v1Response->type());
    ASSERT_TRUE(v1Response->get_executors().executors().empty());
    ASSERT_EQ(1, v1Response->get_executors().completed_executors_size());
  }
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, GetTasks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  ContentType contentType = GetParam();

  // No tasks launched, we should expect zero tasks in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_TASKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_TASKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_tasks().pending_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().queued_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().launched_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().terminated_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().completed_tasks().empty());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // A task launched, we expect one task in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_TASKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_TASKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_tasks().pending_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().queued_tasks().empty());
    ASSERT_EQ(1, v1Response->get_tasks().launched_tasks_size());
    ASSERT_TRUE(v1Response->get_tasks().terminated_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().completed_tasks().empty());
  }

  Clock::pause();

  // Kill the task.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.killTask(statusRunning->task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  // Make sure the agent receives and properly handles the ACK.
  AWAIT_READY(_statusUpdateAcknowledgement);
  Clock::settle();
  Clock::resume();

  // After the executor terminated, we should expect one completed task in
  // Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_TASKS);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_TASKS, v1Response->type());
    ASSERT_TRUE(v1Response->get_tasks().pending_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().queued_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().launched_tasks().empty());
    ASSERT_TRUE(v1Response->get_tasks().terminated_tasks().empty());
    ASSERT_EQ(1, v1Response->get_tasks().completed_tasks_size());
  }

  driver.stop();
  driver.join();
}


TEST_P(AgentAPITest, GetAgent)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  slave::Flags flags = CreateSlaveFlags();
  flags.hostname = "host";
  flags.domain = createDomainInfo("region-xyz", "zone-456");

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_AGENT);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_AGENT, v1Response->type());

  const mesos::v1::AgentInfo& agentInfo = v1Response->get_agent().agent_info();

  ASSERT_EQ(flags.hostname, agentInfo.hostname());
  ASSERT_EQ(evolve(flags.domain.get()), agentInfo.domain());
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, GetState)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  ContentType contentType = GetParam();

  // GetState before task launch, we should expect zero
  // frameworks/tasks/executors in Response.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_STATE);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_STATE, v1Response->type());

    const v1::agent::Response::GetState& getState = v1Response->get_state();
    ASSERT_TRUE(getState.get_frameworks().frameworks().empty());
    ASSERT_TRUE(getState.get_tasks().launched_tasks().empty());
    ASSERT_TRUE(getState.get_executors().executors().empty());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // GetState after task launch and check we have a running
  // framework/task/executor.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_STATE);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_STATE, v1Response->type());

    const v1::agent::Response::GetState& getState = v1Response->get_state();
    ASSERT_EQ(1, getState.get_frameworks().frameworks_size());
    ASSERT_TRUE(getState.get_frameworks().completed_frameworks().empty());
    ASSERT_EQ(1, getState.get_tasks().launched_tasks_size());
    ASSERT_TRUE(getState.get_tasks().completed_tasks().empty());
    ASSERT_EQ(1, getState.get_executors().executors_size());
    ASSERT_TRUE(getState.get_executors().completed_executors().empty());
  }

  Clock::pause();

  // Kill the task.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.killTask(statusRunning->task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  // Make sure the agent receives and properly handles the ACK of `TASK_KILLED`.
  AWAIT_READY(_statusUpdateAcknowledgement);
  Clock::settle();
  Clock::resume();

  // Make sure the executor terminated.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  driver.stop();
  driver.join();

  AWAIT_READY(executorTerminated);

  // Make sure `Framework::destroyExecutor()` is processed.
  Clock::pause();
  Clock::settle();

  // After the executor terminated, we should expect a completed
  // framework/task/executor.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_STATE);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_STATE, v1Response->type());

    const v1::agent::Response::GetState& getState = v1Response->get_state();
    ASSERT_TRUE(getState.get_frameworks().frameworks().empty());
    ASSERT_EQ(1, getState.get_frameworks().completed_frameworks_size());
    ASSERT_TRUE(getState.get_tasks().launched_tasks().empty());
    ASSERT_EQ(1, getState.get_tasks().completed_tasks_size());
    ASSERT_TRUE(getState.get_executors().executors().empty());
    ASSERT_EQ(1, getState.get_executors().completed_executors_size());
  }
}


// Checks that the V1 GET_STATE API will correctly categorize a non-terminal
// task as a completed task, if the task belongs to a completed executor.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest, GetStateWithNonTerminalCompletedTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();

  // Remove this delay so that the agent will immediately kill any tasks.
  slaveFlags.executor_shutdown_grace_period = Seconds(0);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusLost;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusLost));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(offer, "", DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Emulate the checkpointed state of an executor where the following occurs:
  //   1) A graceful shutdown is initiated on the agent (i.e. SIGUSR1).
  //   2) The executor is sent a kill, and starts killing its tasks.
  //   3) The executor exits, before all terminal status updates reach the
  //      agent. This results in a completed executor, with non-terminal tasks.
  //
  // A simple way to reach this state is to shutdown the agent and prevent
  // the executor from sending the appropriate terminal status update.
  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  slave.get()->shutdown();

  AWAIT_READY(shutdown);
  AWAIT_READY(statusLost);

  // Start the agent back up, and allow it to recover the "completed" executor.
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  driver.stop();
  driver.join();

  ContentType contentType = GetParam();

  // Non-terminal tasks on completed executors should appear as terminated
  // tasks, even if they do not have a terminal status update.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_STATE);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_STATE, v1Response->type());

    const v1::agent::Response::GetState& getState = v1Response->get_state();
    EXPECT_TRUE(getState.get_frameworks().frameworks().empty());
    EXPECT_EQ(1, getState.get_frameworks().completed_frameworks_size());
    EXPECT_TRUE(getState.get_tasks().launched_tasks().empty());
    ASSERT_EQ(1, getState.get_tasks().terminated_tasks_size());
    EXPECT_TRUE(getState.get_executors().executors().empty());
    EXPECT_EQ(1, getState.get_executors().completed_executors_size());

    // The latest state of this terminated task will not be terminal,
    // because the executor was not given the chance to send the update.
    EXPECT_EQ(
        v1::TASK_RUNNING, getState.get_tasks().terminated_tasks(0).state());
  }
}


// Checks that the V1 GET_STATE API will correctly categorize a non-terminal
// task as a completed task, if the task belongs to a completed executor.
// This variant of the test introduces the non-terminal task via the
// master TEARDOWN call. A framework TEARDOWN call has similar effects.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest, TeardownAndGetStateWithNonTerminalCompletedTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();

  // Remove this delay so that the agent will immediately kill any tasks.
  slaveFlags.executor_shutdown_grace_period = Seconds(0);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> executorShutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&executorShutdown));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(offer, "", DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ContentType contentType = GetParam();

  // Emulate the checkpointed state of an executor where the following occurs:
  //   1) The operator initiates a TEARDOWN on a running framework.
  //   2) The master tells any affected agents to shutdown the framework.
  //   3) The agent shuts down any executors, the executors exit before
  //      all terminal status updates reach the agent.
  //      This results in a completed executor, with non-terminal tasks.
  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::TEARDOWN);

    v1Call.mutable_teardown()
      ->mutable_framework_id()->set_value(frameworkId->value());

    Future<http::Response> response = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(executorShutdown);

  driver.stop();
  driver.join();

  // Non-terminal tasks on completed executors should appear as terminated
  // tasks, even if they do not have a terminal status update.
  {
    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_STATE);

    Future<v1::agent::Response> v1Response =
      post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_STATE, v1Response->type());

    const v1::agent::Response::GetState& getState = v1Response->get_state();
    EXPECT_TRUE(getState.get_frameworks().frameworks().empty());
    EXPECT_EQ(1, getState.get_frameworks().completed_frameworks_size());
    EXPECT_TRUE(getState.get_tasks().launched_tasks().empty());
    ASSERT_EQ(1, getState.get_tasks().terminated_tasks_size());
    EXPECT_TRUE(getState.get_executors().executors().empty());
    EXPECT_EQ(1, getState.get_executors().completed_executors_size());

    // The latest state of this terminated task will not be terminal,
    // because the executor was not given the chance to send the update.
    EXPECT_EQ(
        v1::TASK_RUNNING, getState.get_tasks().terminated_tasks(0).state());
  }
}


// This test verifies that launch nested container session fails when
// attaching to the output of the container fails. Consequently, the
// launched container should be destroyed.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    LaunchNestedContainerSessionAttachFailure)
{
  ContentType contentType = GetParam();

  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 0.1, 32, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> executorRegistered;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureSatisfy(&executorRegistered));

  EXPECT_CALL(exec, launchTask(_, _));

  driver.start();

  // Trigger authentication and registration for the agent.
  Clock::advance(agentFlags.authentication_backoff_factor);
  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(executorRegistered);

  Future<hashset<ContainerID>> containerIds = containerizer.containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  Future<http::Response> response = http::streaming::post(
    slave.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, call),
    stringify(contentType));

  // Launch should fail because test containerizer doesn't support `attach`.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::InternalServerError().status, response);

  // Settle the clock here to ensure any pending callbacks are executed.
  Clock::settle();

  // Attach failure should result in the destruction of nested container.
  containerIds = containerizer.containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());
  EXPECT_FALSE(containerIds->contains(devolve(containerId)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that launch nested container fails when the parent
// container is unknown to the containerizer.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    LaunchNestedContainerWithUnknownParent)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  TaskInfo taskInfo = createTask(offer, "sleep 1000");

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  // Launch the child container with the random parent ContainerId.
  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(id::UUID::random().toString());

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    call.mutable_launch_nested_container()->mutable_command()
      ->CopyFrom(v1::createCommandInfo("cat"));

    call.mutable_launch_nested_container()->mutable_container()
      ->set_type(mesos::v1::ContainerInfo::MESOS);

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, call),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }

  driver.stop();
  driver.join();
}


// This test verifies that launching a nested container session results
// in stdout and stderr being streamed correctly.
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, LaunchNestedContainerSession)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  // Launch a nested container session that runs a command
  // that writes something to stdout and stderr and exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  string output = "output";
  string error = "error";
  string command = "printf " + output + " && printf " + error + " 1>&2";

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  call.mutable_launch_nested_container_session()->mutable_command()->set_value(
      command);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
    slave.get()->pid,
    "api/v1",
    headers,
    serialize(contentType, call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(stringify(contentType), response->headers.at("Content-Type"));
  ASSERT_NONE(response->headers.get(MESSAGE_CONTENT_TYPE));
  ASSERT_EQ(http::Response::PIPE, response->type);

  ASSERT_SOME(response->reader);
  Future<tuple<string, string>> received =
    getProcessIOData(contentType, response->reader.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  EXPECT_EQ(output, stdoutReceived);
  EXPECT_EQ(error, stderrReceived);

  driver.stop();
  driver.join();
}


// This tests verifies that unauthorized principals are unable to
// launch nested container sessions.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    LaunchNestedContainerSessionUnauthorized)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = true;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> mesosContainerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(mesosContainerizer);

  Owned<slave::Containerizer> containerizer(mesosContainerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  {
    // Default principal is not allowed to launch nested container sessions.
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
      flags.acls->add_launch_nested_container_sessions_under_parent_with_user();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  // Attempt to launch a nested container which does nothing.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  string command = "sleep 1000";

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  call.mutable_launch_nested_container_session()->mutable_command()->set_value(
      command);

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
    slave.get()->pid,
    "api/v1",
    headers,
    serialize(contentType, call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);

  containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  EXPECT_EQ(1u, containerIds->size());

  driver.stop();
  driver.join();
}


// This test verifies that launching a nested container session with `TTYInfo`
// results in stdout and stderr being streamed to the client as stdout.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    LaunchNestedContainerSessionWithTTY)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  // Launch a nested container session that runs a command
  // that writes something to stdout and stderr and exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  string output = "output";
  string error = "error";
  string command = "printf " + output + " && printf " + error + " 1>&2";

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  call.mutable_launch_nested_container_session()->mutable_command()->set_value(
      command);

  call.mutable_launch_nested_container_session()->mutable_container()->set_type(
      mesos::v1::ContainerInfo::MESOS);

  call.mutable_launch_nested_container_session()->mutable_container()
    ->mutable_tty_info();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
    slave.get()->pid,
    "api/v1",
    headers,
    serialize(contentType, call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(stringify(contentType), response->headers.at("Content-Type"));
  ASSERT_EQ(http::Response::PIPE, response->type);

  ASSERT_SOME(response->reader);
  Future<tuple<string, string>> received =
    getProcessIOData(contentType, response->reader.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  EXPECT_EQ(output + error, stdoutReceived);
  EXPECT_EQ("", stderrReceived);

  driver.stop();
  driver.join();
}


// This test verifies that the nested container session is destroyed
// upon a client disconnection.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    LaunchNestedContainerSessionDisconnected)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  // Launch a nested container session that runs `cat` so that it never exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  call.mutable_launch_nested_container_session()->mutable_command()->set_value(
      "cat");

  // TODO(vinod): Ideally, we can use `http::post` here but we cannot currently
  // because the caller currently doesn't have a way to disconnect the
  // connection (e.g., by closing the response reader pipe).
  http::URL agent = http::URL(
      "http",
      slave.get()->pid.address.ip,
      slave.get()->pid.address.port,
      slave.get()->pid.id +
      "/api/v1");

  Future<http::Connection> _connection = http::connect(agent);
  AWAIT_READY(_connection);

  http::Connection connection = _connection.get(); // Remove const.

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);
  headers["Content-Type"] = stringify(contentType);

  http::Request request;
  request.url = agent;
  request.method = "POST";
  request.headers = headers;
  request.body = serialize(contentType, call);

  Future<http::Response> response = connection.send(request, true);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(stringify(contentType), response->headers.at("Content-Type"));
  ASSERT_EQ(http::Response::PIPE, response->type);

  // Disconnect the launch connection. This should
  // result in the nested container being destroyed.
  AWAIT_READY(connection.disconnect());

  AWAIT_READY(containerizer->wait(devolve(containerId)));

  driver.stop();
  driver.join();
}


// This test launches multiple nested container sessions simultaneously for the
// command executor. Each nested container prints a short message to the stdout
// and then terminates. This test verifies that the output of each nested
// container session contains the written message.
//
// TODO(abudnik): Enable this test once MESOS-9257 is resolved.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    DISABLED_ROOT_CGROUPS_LaunchNestedContainerSessionsInParallel)
{
  const int numContainers = 10;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  flags.isolation = "cgroups/all,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  ContentType messageContentType = GetParam();

  // Launch multiple nested container sessions each running a command
  // which writes something to stdout and stderr and then exits.
  vector<Option<http::Pipe::Reader>> outputs;

  for (int i = 0; i < numContainers; i++) {
    containerId.set_value(id::UUID::random().toString());

    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

    call.mutable_launch_nested_container_session()->mutable_container_id()
      ->CopyFrom(containerId);

    call.mutable_launch_nested_container_session()->mutable_command()
      ->CopyFrom(v1::createCommandInfo("echo echo"));

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_ACCEPT] = stringify(messageContentType);

    auto response = http::streaming::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(messageContentType, call),
        stringify(messageContentType));

    ASSERT_SOME(response->reader);

    Option<http::Pipe::Reader> output = response->reader.get();
    ASSERT_SOME(output);
    outputs.emplace_back(std::move(output));
  }

  foreach (Option<http::Pipe::Reader>& output, outputs) {
    // Read the output from the LAUNCH_NESTED_CONTAINER_SESSION.
    ASSERT_SOME(output);

    Future<tuple<string, string>> received =
      getProcessIOData(messageContentType, output.get());

    AWAIT_READY(received);

    string stdoutReceived;
    string stderrReceived;

    tie(stdoutReceived, stderrReceived) = received.get();

    // Verify the output matches what we sent.
    ASSERT_EQ("echo\n", stdoutReceived + stderrReceived);
  }

  driver.stop();
  driver.join();
}


// This test verifies that IOSwitchboard, which holds an open HTTP input
// connection, terminates once IO redirects finish for the corresponding
// nested container:
//   1. Launches a parent container `sleep 1000` via the default executor.
//   2. Launches "sh" as a nested container session.
//   3. Attaches to nested container's input via `ATTACH_CONTAINER_INPUT`
//      call to send "kill `pgrep sleep`" command into "sh" which kills
//      the parent container.
//   4. Check that all containers have been terminated.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    ROOT_CGROUPS_LaunchNestedContainerSessionKillTask)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(subscribed);
  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> updateStarting;
  Future<v1::scheduler::Event::Update> updateRunning;
  Future<v1::scheduler::Event::Update> updateFailed;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
      DoAll(
          FutureArg<1>(&updateStarting),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
      DoAll(
          FutureArg<1>(&updateRunning),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
      DoAll(
          FutureArg<1>(&updateFailed),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
    v1::Resources::parse(defaultTaskResourcesString).get();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());
  ASSERT_TRUE(updateRunning->status().has_container_status());

  v1::ContainerStatus status = updateRunning->status().container_status();

  ASSERT_TRUE(status.has_container_id());
  EXPECT_TRUE(status.container_id().has_parent());

  // Launch "sh" command via `LAUNCH_NESTED_CONTAINER_SESSION` call.
  v1::ContainerID containerId;
  containerId.mutable_parent()->CopyFrom(status.container_id());
  containerId.set_value(id::UUID::random().toString());

  Future<http::Response> sessionResponse;

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

    call.mutable_launch_nested_container_session()->mutable_container_id()
      ->CopyFrom(containerId);

    call.mutable_launch_nested_container_session()->mutable_command()
      ->CopyFrom(v1::createCommandInfo("sh"));

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_ACCEPT] = stringify(contentType);

    sessionResponse = http::streaming::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, sessionResponse);
  }

  // Send "pkill sleep" to "sh" via `ATTACH_CONTAINER_INPUT` call.
  // Note, that we do not close input connection because we want to emulate
  // user's interactive debug session.
  http::Pipe pipe;
  http::Pipe::Writer writer = pipe.writer();
  http::Pipe::Reader reader = pipe.reader();

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
    attach->mutable_container_id()->CopyFrom(containerId);

    writer.write(::recordio::encode(serialize(contentType, call)));
  }

  const std::string command = "pkill sleep\n";

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

    v1::agent::ProcessIO* processIO = attach->mutable_process_io();
    processIO->set_type(v1::agent::ProcessIO::DATA);
    processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
    processIO->mutable_data()->set_data(command);

    writer.write(::recordio::encode(serialize(contentType, call)));
  }

  {
    // TODO(anand): Add a `post()` overload that handles request streaming.
    http::URL agent = http::URL(
        "http",
        slave.get()->pid.address.ip,
        slave.get()->pid.address.port,
        slave.get()->pid.id +
        "/api/v1");

    Future<http::Connection> _connection = http::connect(agent);
    AWAIT_READY(_connection);

    http::Connection connection = _connection.get(); // Remove const.

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Content-Type"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_CONTENT_TYPE] = stringify(contentType);

    http::Request request;
    request.url = agent;
    request.method = "POST";
    request.type = http::Request::PIPE;
    request.reader = reader;
    request.headers = headers;

    Future<http::Response> response = connection.send(request);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Read the output from the LAUNCH_NESTED_CONTAINER_SESSION.
  ASSERT_SOME(sessionResponse->reader);

  Option<http::Pipe::Reader> output = sessionResponse->reader.get();
  ASSERT_SOME(output);

  Future<tuple<string, string>> received =
    getProcessIOData(contentType, output.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  ASSERT_TRUE(stdoutReceived.empty());
  ASSERT_TRUE(stderrReceived.empty());

  // Check terminal status update of the task.
  AWAIT_READY(updateFailed);

  ASSERT_EQ(v1::TASK_FAILED, updateFailed->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFailed->status().task_id());
}


// This test verifies that we can call `ATTACH_CONTAINER_INPUT` more than once.
// We send a short message first, then we send a long message by chunks.
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, AttachContainerInputRepeat)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  // Launch a nested container session that runs `cat`.
  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  v1::agent::Call call;
  call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  call.mutable_launch_nested_container_session()->mutable_container_id()
    ->CopyFrom(containerId);

  call.mutable_launch_nested_container_session()->mutable_command()->set_value(
      "cat");

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
    slave.get()->pid,
    "api/v1",
    headers,
    serialize(contentType, call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(stringify(contentType), response->headers.at("Content-Type"));
  ASSERT_NONE(response->headers.get(MESSAGE_CONTENT_TYPE));
  ASSERT_EQ(http::Response::PIPE, response->type);

  auto attachContainerInput = [&](const std::string& data, bool sendEOF) {
    http::Pipe pipe;
    http::Pipe::Writer writer = pipe.writer();
    http::Pipe::Reader reader = pipe.reader();

    {
      v1::agent::Call call;
      call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

      v1::agent::Call::AttachContainerInput* attach =
        call.mutable_attach_container_input();

      attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
      attach->mutable_container_id()->CopyFrom(containerId);

      writer.write(::recordio::encode(serialize(contentType, call)));
    }

    size_t offset = 0;
    size_t chunkSize = 4096;
    while (offset < data.length()) {
      string dataChunk = data.substr(offset, chunkSize);
      offset += chunkSize;

      v1::agent::Call call;
      call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

      v1::agent::Call::AttachContainerInput* attach =
        call.mutable_attach_container_input();

      attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

      v1::agent::ProcessIO* processIO = attach->mutable_process_io();
      processIO->set_type(v1::agent::ProcessIO::DATA);
      processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
      processIO->mutable_data()->set_data(dataChunk);

      writer.write(::recordio::encode(serialize(contentType, call)));
    }

    // Signal `EOF` to the 'cat' command.
    if (sendEOF) {
      v1::agent::Call call;
      call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

      v1::agent::Call::AttachContainerInput* attach =
        call.mutable_attach_container_input();

      attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

      v1::agent::ProcessIO* processIO = attach->mutable_process_io();
      processIO->set_type(v1::agent::ProcessIO::DATA);
      processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
      processIO->mutable_data()->set_data("");

      writer.write(::recordio::encode(serialize(contentType, call)));
    }

    writer.close();

    // TODO(anand): Add a `post()` overload that handles request streaming.
    {
      http::URL agent = http::URL(
          "http",
          slave.get()->pid.address.ip,
          slave.get()->pid.address.port,
          slave.get()->pid.id +
          "/api/v1");

      Future<http::Connection> _connection = http::connect(agent);
      AWAIT_READY(_connection);

      http::Connection connection = _connection.get(); // Remove const.

      http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
      headers["Content-Type"] = stringify(ContentType::RECORDIO);
      headers[MESSAGE_CONTENT_TYPE] = stringify(contentType);

      http::Request request;
      request.url = agent;
      request.method = "POST";
      request.type = http::Request::PIPE;
      request.reader = reader;
      request.headers = headers;

      Future<http::Response> response = connection.send(request);
      AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
    }
  };

  // Prepare the data to send to `cat` and send it over an
  // `ATTACH_CONTAINER_INPUT` stream.
  string data1 = "Hello, World!";
  string data2 =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data2.size()) < Megabytes(1)) {
    data2.append(data2);
  }

  attachContainerInput(data1, false);
  attachContainerInput(data2, true);

  ASSERT_SOME(response->reader);
  Future<tuple<string, string>> received =
    getProcessIOData(contentType, response->reader.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  EXPECT_EQ(stdoutReceived, data1 + data2);

  ASSERT_TRUE(stderrReceived.empty());

  driver.stop();
  driver.join();
}


// This test verifies that attaching to the output of a container fails if the
// containerizer doesn't support the operation.
TEST_P(AgentAPITest, AttachContainerOutputFailure)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // Disable authorization in the agent.
  flags.acls = None();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  Future<hashset<ContainerID>> containerIds = containerizer.containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::agent::Call call;
  call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

  call.mutable_attach_container_output()->mutable_container_id()
    ->set_value(containerIds->begin()->value());

  EXPECT_CALL(containerizer, attach(_))
    .WillOnce(Return(process::Failure("Unsupported")));

  Future<http::Response> response = http::post(
    slave.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::InternalServerError().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("Unsupported", response);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that attaching to the input of a container fails if the
// containerizer doesn't support the operation.
TEST_F(AgentAPITest, AttachContainerInputFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // Disable authorization in the agent.
  flags.acls = None();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  Future<hashset<ContainerID>> containerIds = containerizer.containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::agent::Call call;
  call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

  call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::CONTAINER_ID);

  call.mutable_attach_container_input()->mutable_container_id()
    ->set_value(containerIds->begin()->value());

  ContentType contentType = ContentType::RECORDIO;
  ContentType messageContentType = ContentType::PROTOBUF;

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

  EXPECT_CALL(containerizer, attach(_))
    .WillOnce(Return(process::Failure("Unsupported")));

  Future<http::Response> response = http::post(
    slave.get()->pid,
    "api/v1",
    headers,
    ::recordio::encode(serialize(messageContentType, call)),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::InternalServerError().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("Unsupported", response);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Verifies that unauthorized users are not able to attach to a
// nested container input.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPITest,
    AttachContainerInputAuthorization)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = true;

  {
    mesos::ACL::AttachContainerInput* acl =
      flags.acls->add_attach_containers_input();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  // Launch a nested container session which runs a shell.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  {
    string command = "sh";

    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

    call.mutable_launch_nested_container_session()->mutable_container_id()
        ->CopyFrom(containerId);

    call.mutable_launch_nested_container_session()
        ->mutable_command()->set_value(command);

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<http::Response> response = http::streaming::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Attempt to attach to the container session's input.

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()
        ->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);

    call.mutable_attach_container_input()->mutable_container_id()
        ->CopyFrom(containerId);

    ContentType contentType = ContentType::RECORDIO;
    ContentType messageContentType = ContentType::PROTOBUF;

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      ::recordio::encode(serialize(messageContentType, call)),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  driver.stop();
  driver.join();
}


TEST_F(AgentAPITest, AttachContainerInputValidation)
{
  Clock::pause();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);

  ASSERT_SOME(slave);

  // Wait for the agent to finish recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  ContentType contentType = ContentType::RECORDIO;
  ContentType messageContentType = ContentType::PROTOBUF;

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

  // Missing 'attach_container_input.container_id'.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::CONTAINER_ID);

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        ::recordio::encode(serialize(messageContentType, call)),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }

  // First call on the request stream should be of type
  // 'AttachContainerInput::CONTAINER_ID'.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::PROCESS_IO);

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        ::recordio::encode(serialize(messageContentType, call)),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }
}


// This test verifies that any missing headers or unsupported media
// types in the request result in a 4xx response.
TEST_F(AgentAPITest, HeaderValidation)
{
  Clock::pause();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);

  ASSERT_SOME(slave);

  // Wait for the agent to finish recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  // Missing 'Message-Content-Type' header for a streaming request.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::CONTAINER_ID);

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        ::recordio::encode(serialize(ContentType::PROTOBUF, call)),
        stringify(ContentType::RECORDIO));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }

  // Unsupported 'Message-Content-Type' media type for a streaming request.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::CONTAINER_ID);

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers[MESSAGE_CONTENT_TYPE] = "unsupported/media-type";

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        ::recordio::encode(serialize(ContentType::PROTOBUF, call)),
        stringify(ContentType::RECORDIO));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::UnsupportedMediaType().status,
                                    response);
  }

  // Unsupported 'Message-Accept' media type for a streaming response.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

    v1::ContainerID containerId;
    containerId.set_value(id::UUID::random().toString());

    call.mutable_attach_container_output()->mutable_container_id()
      ->CopyFrom(containerId);

    ContentType contentType = ContentType::PROTOBUF;

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_ACCEPT] = "unsupported/media-type";

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotAcceptable().status, response);
  }

  // Setting 'Message-Content-Type' header for a non-streaming request.
  {
    v1::ContainerID containerId;
    containerId.set_value(id::UUID::random().toString());

    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

    call.mutable_attach_container_output()->mutable_container_id()
      ->CopyFrom(containerId);

    ContentType contentType = ContentType::PROTOBUF;

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers[MESSAGE_CONTENT_TYPE] = stringify(ContentType::PROTOBUF);

    Future<http::Response> response = http::streaming::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::UnsupportedMediaType().status,
                                    response);
  }

  // Setting 'Accept: application/recordio' header for a non-streaming request.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::GET_CONTAINERS);

    ContentType contentType = ContentType::JSON;

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = "application/recordio";

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotAcceptable().status,
                                    response);
  }
}


// This test verifies that the default 'Accept' for the
// Agent API endpoint is `APPLICATION_JSON`.
TEST_P(AgentAPITest, DefaultAccept)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait until the agent has finished recovery.
  Clock::pause();
  Clock::settle();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = "*/*";

  v1::agent::Call call;
  call.set_type(v1::agent::Call::GET_STATE);

  ContentType contentType = GetParam();

  Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
}


TEST_P(AgentAPITest, GetResourceProviders)
{
  Clock::pause();

  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.authenticate_http_readwrite = true;

  {
    // `DEFAULT_CREDENTIAL_2` is not allowed to view any resource providers.
    mesos::ACL::ViewResourceProvider* acl =
      slaveFlags.acls->add_view_resource_providers();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_RESOURCE_PROVIDERS);

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_RESOURCE_PROVIDERS, v1Response->type());

  EXPECT_TRUE(
      v1Response->get_resource_providers().resource_providers().empty());

  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  v1::Resource resource = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  v1::TestResourceProvider resourceProvider(info, resource);

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  v1Response = post(slave.get()->pid, v1Call, contentType, DEFAULT_CREDENTIAL);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_RESOURCE_PROVIDERS, v1Response->type());
  EXPECT_EQ(1, v1Response->get_resource_providers().resource_providers_size());

  const mesos::v1::ResourceProviderInfo& responseInfo =
    v1Response->get_resource_providers()
      .resource_providers(0)
      .resource_provider_info();

  EXPECT_EQ(info.type(), responseInfo.type());
  EXPECT_EQ(info.name(), responseInfo.name());

  ASSERT_TRUE(responseInfo.has_id());
  resource.mutable_provider_id()->CopyFrom(responseInfo.id());

  const v1::Resources responseResources =
    v1Response->get_resource_providers()
      .resource_providers(0)
      .total_resources();

  EXPECT_EQ(v1::Resources(resource), responseResources);

  // `DEFAULT_CREDENTIAL_2` is not allow to view any resource provider
  // information.
  v1Response =
    post(slave.get()->pid, v1Call, contentType, DEFAULT_CREDENTIAL_2);
  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_RESOURCE_PROVIDERS, v1Response->type());
  EXPECT_EQ(0, v1Response->get_resource_providers().resource_providers_size());
}


TEST_P(AgentAPITest, MarkResourceProviderGone)
{
  Clock::pause();

  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.authenticate_http_readwrite = true;

  {
    // Default principal 2 is not allowed to mark any resource provider gone.
    mesos::ACL::MarkResourceProvidersGone* acl =
      slaveFlags.acls->add_mark_resource_providers_gone();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  // Start a resource provider without resources since resource
  // providers with resources cannot be marked gone.
  v1::TestResourceProvider resourceProvider(info, v1::Resource());

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  Future<mesos::v1::resource_provider::Event::Subscribed> subscribed;

  auto resourceProviderProcess = resourceProvider.process->self();
  EXPECT_CALL(*resourceProvider.process, subscribed(_))
    .WillOnce(DoAll(
        Invoke(
            [resourceProviderProcess](
                const typename mesos::v1::resource_provider::Event::Subscribed&
                  subscribed) {
              dispatch(
                  resourceProviderProcess,
                  &v1::TestResourceProviderProcess::subscribedDefault,
                  subscribed);
            }),
        FutureArg<0>(&subscribed)))
    .WillRepeatedly(Return());

  // After the resource provider it will update its resources which
  // triggers an `UpdateSlaveMessage`.
  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), contentType);

  AWAIT_READY(subscribed);
  AWAIT_READY(updateSlaveMessage);

  const mesos::v1::ResourceProviderID& resourceProviderId =
    subscribed->provider_id();

  // Removing the resource provider should trigger an `UpdateSlaveMessage`.
  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Mark the resource provider gone.
  {
    // We explicitly track whether the resource provider was disconnected as it
    // indicates whether the agent has cleaned up the connection to the
    // provider.
    //
    // NOTE: As the resource provider driver will try to resubscribe if the
    // connection is broken will does not succeed, we might observe other
    // `disconnected` events.
    Future<Nothing> disconnected;
    EXPECT_CALL(*resourceProvider.process, disconnected())
      .WillOnce(FutureSatisfy(&disconnected))
      .WillRepeatedly(Return());

    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::MARK_RESOURCE_PROVIDER_GONE);
    v1Call.mutable_mark_resource_provider_gone()
      ->mutable_resource_provider_id()
      ->CopyFrom(resourceProviderId);

    // `DEFAULT_CREDENTIAL_2` is not able to mark the resource provider as gone.
    http::Headers headers2 = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
    headers2["Accept"] = stringify(contentType);

    Future<http::Response> v1Response = http::post(
        slave.get()->pid,
        "api/v1",
        headers2,
        serialize(contentType, v1Call),
        stringify(contentType));
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, v1Response);

    // Other principals are able to remove the resource provider.
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    v1Response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, v1Call),
        stringify(contentType));
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, v1Response);

    AWAIT_READY(disconnected);
  }

  // Verify that resource provider is not be there anymore.
  {
    AWAIT_READY(updateSlaveMessage);

    v1::agent::Call v1Call;
    v1Call.set_type(v1::agent::Call::GET_RESOURCE_PROVIDERS);

    auto v1Response = post(slave.get()->pid, v1Call, contentType);

    AWAIT_READY(v1Response);
    ASSERT_TRUE(v1Response->IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_RESOURCE_PROVIDERS, v1Response->type());

    EXPECT_TRUE(
        v1Response->get_resource_providers().resource_providers().empty());
  }
}


TEST_P(AgentAPITest, GetOperations)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.authenticate_http_readwrite = true;

  {
    // Default principal 2 is not allowed to view any role.
    mesos::ACL::ViewRole* acl = slaveFlags.acls->add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo info;
  info.set_type("org.apache.mesos.rp.test");
  info.set_name("test");

  v1::TestResourceProvider resourceProvider(
      info,
      v1::createDiskResource(
          "200",
          "*",
          None(),
          None(),
          v1::createDiskSourceRaw(None(), "profile")));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  const ContentType contentType = GetParam();

  resourceProvider.start(std::move(endpointDetector), contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_OPERATIONS);

  Future<v1::agent::Response> v1Response =
    post(agent.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_TRUE(v1Response->get_operations().operations().empty());

  // Start a framework to operate on offers.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // We settle here to make sure that the framework has been authenticated
  // before advancing the clock. Otherwise we would run into a authentication
  // timeout due to the large allocation interval (1000s) of this fixture.
  Clock::settle();

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer& offer = offers->front();

  Option<Resource> rawDisk;

  foreach (const Resource& resource, offer.resources()) {
    if (resource.has_provider_id() &&
        resource.has_disk() &&
        resource.disk().has_source() &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      rawDisk = resource;
      break;
    }
  }

  ASSERT_SOME(rawDisk);

  // The operation is still pending when we receive this event.
  Future<mesos::v1::resource_provider::Event::ApplyOperation> operation;
  EXPECT_CALL(*resourceProvider.process, applyOperation(_))
    .WillOnce(FutureArg<0>(&operation));

  // Start an operation.
  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(rawDisk.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(operation);

  v1Response = post(agent.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_EQ(1, v1Response->get_operations().operations_size());
  EXPECT_EQ(
      operation->framework_id(),
      v1Response->get_operations().operations(0).framework_id());
  EXPECT_EQ(
      evolve(updateSlaveMessage->slave_id()),
      v1Response->get_operations().operations(0).agent_id());
  EXPECT_EQ(
      operation->info(), v1Response->get_operations().operations(0).info());
  EXPECT_EQ(
      operation->operation_uuid(),
      v1Response->get_operations().operations(0).uuid());

  // Default principal 2 should not be able to see any operations.
  v1Response =
    post(agent.get()->pid, v1Call, contentType, DEFAULT_CREDENTIAL_2);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_OPERATIONS, v1Response->type());
  EXPECT_TRUE(v1Response->get_operations().operations().empty());

  driver.stop();
  driver.join();
}


class AgentAPIStreamingTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// These tests are parameterized by the content type of the
// streaming HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    AgentAPIStreamingTest,
    ::testing::Values(
        ContentType::PROTOBUF, ContentType::JSON));


// This test launches a child container with TTY and the 'cat' command
// as the entrypoint and attaches to its STDOUT via the attach output call.
// It then verifies that any data streamed to the container via the
// attach input call is received by the client on the output stream.
//
// TODO(alexr): Enable this test once MESOS-6780 is resolved.
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPIStreamingTest,
                                DISABLED_AttachContainerInput)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo taskInfo = createTask(offer, "sleep 1000");

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  // Launch the child container with TTY and then attach to it's output.

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    call.mutable_launch_nested_container()->mutable_command()
      ->CopyFrom(v1::createCommandInfo("cat"));

    call.mutable_launch_nested_container()->mutable_container()
      ->set_type(mesos::v1::ContainerInfo::MESOS);

    call.mutable_launch_nested_container()->mutable_container()
      ->mutable_tty_info();

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, call),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  ContentType messageContentType = GetParam();

  Option<http::Pipe::Reader> output;

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

    call.mutable_attach_container_output()->mutable_container_id()
      ->CopyFrom(containerId);

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(messageContentType);

    Future<http::Response> response = http::streaming::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(messageContentType, call),
        stringify(messageContentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
    ASSERT_SOME(response->reader);

    output = response->reader.get();
  }

  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.\n";

  // Terminal transforms "\n" to "\r\n".
  string stdoutExpected = strings::trim(data) + "\r\n";

  http::Pipe pipe;
  http::Pipe::Writer writer = pipe.writer();
  http::Pipe::Reader reader = pipe.reader();

  // Prepare the data that needs to be streamed to the entrypoint
  // of the container.

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
    attach->mutable_container_id()->CopyFrom(containerId);

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  size_t offset = 0;
  size_t chunkSize = 4096;
  while (offset < data.length()) {
    string dataChunk = data.substr(offset, chunkSize);
    offset += chunkSize;

    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

    v1::agent::ProcessIO* processIO = attach->mutable_process_io();
    processIO->set_type(v1::agent::ProcessIO::DATA);
    processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
    processIO->mutable_data()->set_data(dataChunk);

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  // Signal `EOT` to the terminal so that it sends `EOF` to `cat` command.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

    v1::agent::ProcessIO* processIO = attach->mutable_process_io();
    processIO->set_type(v1::agent::ProcessIO::DATA);
    processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
    processIO->mutable_data()->set_data("\x04");

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  writer.close();

  // TODO(anand): Add a `post()` overload that handles request streaming.
  {
    http::URL agent = http::URL(
        "http",
        slave.get()->pid.address.ip,
        slave.get()->pid.address.port,
        slave.get()->pid.id +
        "/api/v1");

    Future<http::Connection> _connection = http::connect(agent);
    AWAIT_READY(_connection);

    http::Connection connection = _connection.get(); // Remove const.

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Content-Type"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

    http::Request request;
    request.url = agent;
    request.method = "POST";
    request.type = http::Request::PIPE;
    request.reader = reader;
    request.headers = headers;

    Future<http::Response> response = connection.send(request);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  ASSERT_SOME(output);

  Future<tuple<string, string>> received =
    getProcessIOData(messageContentType, output.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  // `stdoutExpected` appears twice in stdout because the terminal in raw mode
  // echoes the data once and `cat` outputs it once.
  ASSERT_EQ(stdoutExpected + stdoutExpected, stdoutReceived);

  ASSERT_TRUE(stderrReceived.empty());
}


// This test launches a nested container session with 'cat' as its
// entrypoint and verifies that any data streamed to the container via
// an ATTACH_CONTAINER_INPUT call is received by the client on the
// output stream.
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    AgentAPIStreamingTest,
    AttachInputToNestedContainerSession)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  TaskInfo taskInfo = createTask(offer, "sleep 1000");

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  EXPECT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  ContentType messageContentType = GetParam();

  Future<http::Response> sessionResponse;

  // Start a new LAUNCH_NESTED_CONTAINER_SESSION with `cat` as the
  // command being launched.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

    call.mutable_launch_nested_container_session()->mutable_container_id()
      ->CopyFrom(containerId);

    call.mutable_launch_nested_container_session()->mutable_command()
      ->CopyFrom(v1::createCommandInfo("cat"));

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_ACCEPT] = stringify(messageContentType);

    sessionResponse = http::streaming::post(
        slave.get()->pid,
        "api/v1",
        headers,
        serialize(messageContentType, call),
        stringify(messageContentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, sessionResponse);
  }

  // Prepare the data to send to `cat` and send it over an
  // `ATTACH_CONTAINER_INPUT` stream.
  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data.size()) < Megabytes(1)) {
    data.append(data);
  }

  http::Pipe pipe;
  http::Pipe::Writer writer = pipe.writer();
  http::Pipe::Reader reader = pipe.reader();

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
    attach->mutable_container_id()->CopyFrom(containerId);

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  size_t offset = 0;
  size_t chunkSize = 4096;
  while (offset < data.length()) {
    string dataChunk = data.substr(offset, chunkSize);
    offset += chunkSize;

    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

    v1::agent::ProcessIO* processIO = attach->mutable_process_io();
    processIO->set_type(v1::agent::ProcessIO::DATA);
    processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
    processIO->mutable_data()->set_data(dataChunk);

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  // Signal `EOF` to the 'cat' command.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::PROCESS_IO);

    v1::agent::ProcessIO* processIO = attach->mutable_process_io();
    processIO->set_type(v1::agent::ProcessIO::DATA);
    processIO->mutable_data()->set_type(v1::agent::ProcessIO::Data::STDIN);
    processIO->mutable_data()->set_data("");

    writer.write(::recordio::encode(serialize(messageContentType, call)));
  }

  writer.close();

  {
    // TODO(anand): Add a `post()` overload that handles request streaming.
    http::URL agent = http::URL(
        "http",
        slave.get()->pid.address.ip,
        slave.get()->pid.address.port,
        slave.get()->pid.id +
        "/api/v1");

    Future<http::Connection> _connection = http::connect(agent);
    AWAIT_READY(_connection);

    http::Connection connection = _connection.get(); // Remove const.

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Content-Type"] = stringify(ContentType::RECORDIO);
    headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

    http::Request request;
    request.url = agent;
    request.method = "POST";
    request.type = http::Request::PIPE;
    request.reader = reader;
    request.headers = headers;

    Future<http::Response> response = connection.send(request);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Read the output from the LAUNCH_NESTED_CONTAINER_SESSION.
  ASSERT_SOME(sessionResponse->reader);

  Option<http::Pipe::Reader> output = sessionResponse->reader.get();
  ASSERT_SOME(output);

  Future<tuple<string, string>> received =
    getProcessIOData(messageContentType, output.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  // Verify the output matches what we sent.
  ASSERT_TRUE(stderrReceived.empty());
  ASSERT_EQ(data, stdoutReceived);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
