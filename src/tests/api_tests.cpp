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

#include <string>
#include <tuple>

#include <mesos/http.hpp>

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
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/recordio.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

#include "tests/containerizer/mock_containerizer.hpp"

namespace http = process::http;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::slave::ContainerTermination;

using mesos::internal::devolve;
using mesos::internal::evolve;

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

using std::string;
using std::tuple;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class MasterAPITest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  // Helper function to post a request to "/api/v1" master endpoint and return
  // the response.
  Future<v1::master::Response> post(
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
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
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start one agent.
  Future<SlaveRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags flags = CreateSlaveFlags();
  flags.hostname = "host";

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), flags);
  ASSERT_SOME(agent);

  AWAIT_READY(agentRegisteredMessage);

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

  ASSERT_EQ("host", v1Agent.agent_info().hostname());
  ASSERT_EQ(agent.get()->pid, v1Agent.pid());
  ASSERT_TRUE(v1Agent.active());
  ASSERT_EQ(MESOS_VERSION, v1Agent.version());
  ASSERT_EQ(4, v1Agent.total_resources_size());
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
  ASSERT_EQ("*", frameworks.frameworks(0).framework_info().role());
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
  ASSERT_LE(0, metrics.size());
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
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

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
  EXPECT_NE(0u, offers->size());

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

  EXPECT_NE(0u, offers->size());

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
    ASSERT_EQ(1u, getState.get_frameworks().frameworks_size());
    ASSERT_EQ(1u, getState.get_agents().agents_size());
    ASSERT_EQ(0u, getState.get_tasks().tasks_size());
    ASSERT_EQ(0u, getState.get_executors().executors_size());
  }

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

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
    ASSERT_EQ(1u, getState.get_tasks().tasks_size());
    ASSERT_EQ(0u, getState.get_tasks().completed_tasks_size());
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
    ASSERT_EQ(1u, getState.get_tasks().completed_tasks_size());
    ASSERT_EQ(0u, getState.get_tasks().tasks_size());
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

  ASSERT_EQ(0, v1Response->get_tasks().pending_tasks().size());
  ASSERT_EQ(0, v1Response->get_tasks().tasks().size());
  ASSERT_EQ(0, v1Response->get_tasks().completed_tasks().size());
  ASSERT_EQ(0, v1Response->get_tasks().orphan_tasks().size());
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
  EXPECT_NE(0u, offers->size());

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
    ASSERT_EQ(0, v1Response->get_tasks().tasks().size());
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
  Try<Owned<cluster::Master>> master = this->StartMaster();
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
  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<Nothing> v1Response = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1Response);
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
  frameworkInfo.set_role("role1");

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
  ASSERT_EQ(
      allocatedResources(
          devolve(v1::Resources::parse(slaveFlags.resources.get()).get()),
          "role1"),
      devolve(v1Response->get_roles().roles(1).resources()));

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetMaster)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_MASTER);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_MASTER, v1Response->type());
  ASSERT_EQ(master.get()->getMasterInfo().ip(),
            v1Response->get_master().master_info().ip());
}


// This test verifies that an operator can reserve available resources through
// the `RESERVE_RESOURCES` call.
TEST_P(MasterAPITest, ReserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  // Set a low allocation interval to speed up this test.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, flags);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal())).get();

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
      allocatedResources(unreserved, frameworkInfo.role())));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

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

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test verifies that an operator can unreserve reserved resources through
// the `UNRESERVE_RESOURCES` call.
TEST_P(MasterAPITest, UnreserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  // Set a low allocation interval to speed up this test.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(&allocator, flags);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal())).get();

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
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

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

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  // Verifies if the resources are unreserved.
  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// Test updates a maintenance schedule and verifies it saved via query.
TEST_P(MasterAPITest, UpdateAndGetMaintenanceSchedule)
{
  // Set up a master.
  Try<Owned<cluster::Master>> master = this->StartMaster();
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

  Future<Nothing> v1UpdateScheduleResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1UpdateScheduleResponse);

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

  // Verify maintenance schedule matches the expectation.
  v1::maintenance::Schedule respSchedule =
    v1GetScheduleResponse->get_maintenance_schedule().schedule();
  ASSERT_EQ(1, respSchedule.windows().size());
  ASSERT_EQ(2, respSchedule.windows(0).machine_ids().size());
  ASSERT_EQ("Machine1", respSchedule.windows(0).machine_ids(0).hostname());
  ASSERT_EQ("0.0.0.2", respSchedule.windows(0).machine_ids(1).ip());
}


// Test queries for machine maintenance status.
TEST_P(MasterAPITest, GetMaintenanceStatus)
{
  // Set up a master.
  Try<Owned<cluster::Master>> master = this->StartMaster();
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

  Future<Nothing> v1UpdateScheduleResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1UpdateScheduleResponse);

  // Query maintenance status.
  v1::master::Call v1GetStatusCall;
  v1GetStatusCall.set_type(v1::master::Call::GET_MAINTENANCE_STATUS);

  Future<v1::master::Response> v1GetStatusResponse =
    post(master.get()->pid, v1GetStatusCall, contentType);

  AWAIT_READY(v1GetStatusResponse);
  ASSERT_TRUE(v1GetStatusResponse->IsInitialized());
  ASSERT_EQ(
      v1::master::Response::GET_MAINTENANCE_STATUS,
      v1GetStatusResponse->type());

  // Verify maintenance status matches the expectation.
  v1::maintenance::ClusterStatus status =
    v1GetStatusResponse->get_maintenance_status().status();
  ASSERT_EQ(2, status.draining_machines().size());
  ASSERT_EQ(0, status.down_machines().size());
}


// Test start machine maintenance and stop machine maintenance APIs.
// In this test case, we start maintenance on a machine and stop maintenance,
// and then verify that the associated maintenance window disappears.
TEST_P(MasterAPITest, StartAndStopMaintenance)
{
  // Set up a master.
  Try<Owned<cluster::Master>> master = this->StartMaster();
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

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  Future<Nothing> v1UpdateScheduleResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1UpdateScheduleResponse);

  // Start maintenance on machine3.
  v1::master::Call v1StartMaintenanceCall;
  v1StartMaintenanceCall.set_type(v1::master::Call::START_MAINTENANCE);
  v1::master::Call_StartMaintenance* startMaintenance =
    v1StartMaintenanceCall.mutable_start_maintenance();
  startMaintenance->add_machines()->CopyFrom(evolve(machine3));

  Future<Nothing> v1StartMaintenanceResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1StartMaintenanceCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1StartMaintenanceResponse);

  // Stop maintenance on machine3.
  v1::master::Call v1StopMaintenanceCall;
  v1StopMaintenanceCall.set_type(v1::master::Call::STOP_MAINTENANCE);
  v1::master::Call_StopMaintenance* stopMaintenance =
    v1StopMaintenanceCall.mutable_stop_maintenance();
  stopMaintenance->add_machines()->CopyFrom(evolve(machine3));

  Future<Nothing> v1StopMaintenanceResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1StopMaintenanceCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1StopMaintenanceResponse);

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

  Reader<v1::master::Event> decoder(
      Decoder<v1::master::Event>(deserializer), reader);

  Future<Result<v1::master::Event>> event = decoder.read();
  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  const v1::master::Response::GetState& getState =
      event->get().subscribed().get_state();

  EXPECT_EQ(0u, getState.get_frameworks().frameworks_size());
  EXPECT_EQ(0u, getState.get_agents().agents_size());
  EXPECT_EQ(0u, getState.get_tasks().tasks_size());
  EXPECT_EQ(0u, getState.get_executors().executors_size());

  // Start one agent.
  Future<SlaveRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags flags = CreateSlaveFlags();
  flags.hostname = "host";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(agentRegisteredMessage);

  event = decoder.read();
  AWAIT_READY(event);

  SlaveID agentID = agentRegisteredMessage->slave_id();

  {
    ASSERT_EQ(v1::master::Event::AGENT_ADDED, event->get().type());

    const v1::master::Response::GetAgents::Agent& agent =
      event->get().agent_added().agent();

    ASSERT_EQ("host", agent.agent_info().hostname());
    ASSERT_EQ(evolve(agentID), agent.agent_info().id());
    ASSERT_EQ(slave.get()->pid, agent.pid());
    ASSERT_EQ(MESOS_VERSION, agent.version());
    ASSERT_EQ(4, agent.total_resources_size());
  }

  // Forcefully trigger a shutdown on the slave so that master will remove it.
  slave.get()->shutdown();
  slave->reset();

  event = decoder.read();
  AWAIT_READY(event);

  {
    ASSERT_EQ(v1::master::Event::AGENT_REMOVED, event->get().type());
    ASSERT_EQ(evolve(agentID), event->get().agent_removed().agent_id());
  }
}


// This test verifies that recovered but yet to reregister agents are returned
// in `recovered_agents` field of `GetAgents` response.
TEST_P_TEMP_DISABLED_ON_WINDOWS(MasterAPITest, GetRecoveredAgents)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  v1::AgentID agentId = evolve(slaveRegisteredMessage->slave_id());

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
    ASSERT_EQ(0, v1Response->get_agents().recovered_agents_size());
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
    ASSERT_EQ(0u, v1Response->get_agents().agents_size());
    ASSERT_EQ(1u, v1Response->get_agents().recovered_agents_size());
    ASSERT_EQ(agentId, v1Response->get_agents().recovered_agents(0).id());
  }

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Start the agent to make it re-register with the master.
  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // After the agent has successfully re-registered with the master,
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
    ASSERT_EQ(1u, v1Response->get_agents().agents_size());
    ASSERT_EQ(0u, v1Response->get_agents().recovered_agents_size());
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

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::SUBSCRIBE);

    v1::scheduler::Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  // Launch a task using the scheduler. This should result in a `TASK_ADDED`
  // event when the task is launched followed by a `TASK_UPDATED` event after
  // the task transitions to running state.
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  // Create event stream after seeing first offer but before first task is
  // launched. We should see one framework, one agent and zero task/executor.
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

  Reader<v1::master::Event> decoder(
      Decoder<v1::master::Event>(deserializer), reader);

  Future<Result<v1::master::Event>> event = decoder.read();
  AWAIT_READY(event);

  EXPECT_EQ(v1::master::Event::SUBSCRIBED, event->get().type());
  const v1::master::Response::GetState& getState =
      event->get().subscribed().get_state();

  EXPECT_EQ(1u, getState.get_frameworks().frameworks_size());
  EXPECT_EQ(1u, getState.get_agents().agents_size());
  EXPECT_EQ(0u, getState.get_tasks().tasks_size());
  EXPECT_EQ(0u, getState.get_executors().executors_size());

  event = decoder.read();
  EXPECT_TRUE(event.isPending());

  const v1::Offer& offer = offers->offers(0);

  TaskInfo task = createTask(devolve(offer), "", executorId);

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

  AWAIT_READY(event);
  AWAIT_READY(update);

  ASSERT_EQ(v1::master::Event::TASK_ADDED, event->get().type());
  ASSERT_EQ(evolve(task.task_id()),
            event->get().task_added().task().task_id());

  event = decoder.read();

  AWAIT_READY(event);

  ASSERT_EQ(v1::master::Event::TASK_UPDATED, event->get().type());
  ASSERT_EQ(v1::TASK_RUNNING,
            event->get().task_updated().state());
  ASSERT_EQ(v1::TASK_RUNNING,
            event->get().task_updated().status().state());
  ASSERT_EQ(evolve(task.task_id()),
            event->get().task_updated().status().task_id());

  event = decoder.read();

  Future<Nothing> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureSatisfy(&update2));

  // After we advance the clock, the status update manager would retry the
  // `TASK_RUNNING` update. Since, the state of the task is not changed, this
  // should not result in another `TASK_UPDATED` event.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  AWAIT_READY(update2);

  EXPECT_TRUE(event.isPending());

  EXPECT_TRUE(reader.close());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
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
    ASSERT_EQ(0, v1Response->get_quota().status().infos().size());
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

  Future<Nothing> v1CreateVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::Accepted().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1CreateVolumesResponse);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

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
  EXPECT_NE(0u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));

  Resources taskResources = Resources::parse(
      "disk:256",
      frameworkInfo.role()).get();

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

  Future<Nothing> v1DestroyVolumesResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1DestroyVolumesCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::Accepted().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1DestroyVolumesResponse);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
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

  Future<Nothing> updateResponse = http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, updateCall),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(updateResponse);

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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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


class AgentAPITest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  // Helper function to post a request to "/api/v1" agent endpoint and return
  // the response.
  Future<v1::agent::Response> post(
      const process::PID<slave::Slave>& pid,
      const v1::agent::Call& call,
      const ContentType& contentType)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
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

      ::recordio::Decoder<v1::agent::ProcessIO> decoder(lambda::bind(
          deserialize<v1::agent::ProcessIO>, contentType, lambda::_1));

      Try<std::deque<Try<v1::agent::ProcessIO>>> records =
        decoder.decode(data);

      if (records.isError()) {
        return process::Failure(records.error());
      }

      while(!records->empty()) {
        Try<v1::agent::ProcessIO> record = records->front();
        records->pop_front();

        if (record.isError()) {
          return process::Failure(record.error());
        }

        if (record->data().type() == v1::agent::ProcessIO::Data::STDOUT) {
          stdoutReceived += record->data().data();
        } else if (record->data().type() ==
            v1::agent::ProcessIO::Data::STDERR) {
          stderrReceived += record->data().data();
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
  ASSERT_LE(0, metrics.size());
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

  Future<Nothing> v1Response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType))
    .then([](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status) {
        return Failure("Unexpected response status " + response.status);
      }
      return Nothing();
    });

  AWAIT_READY(v1Response);
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
  EXPECT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      "sleep 1000",
      exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

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
    ASSERT_EQ(0, v1Response->get_containers().containers_size());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

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
    .WillOnce(Return(containerStatus));

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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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
    ASSERT_EQ(4, v1Response->read_file().size());
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
  EXPECT_NE(0u, offers->size());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
    ASSERT_EQ(0, v1Response->get_frameworks().frameworks_size());
    ASSERT_EQ(0, v1Response->get_frameworks().completed_frameworks_size());
  }

  driver.launchTasks(offer.id(), {task});

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
    ASSERT_EQ(0, v1Response->get_frameworks().completed_frameworks_size());
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
    ASSERT_EQ(0, v1Response->get_frameworks().frameworks_size());
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
  EXPECT_NE(0u, offers->size());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
    ASSERT_EQ(0, v1Response->get_executors().executors_size());
    ASSERT_EQ(0, v1Response->get_executors().completed_executors_size());
  }

  driver.launchTasks(offer.id(), {task});

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
    ASSERT_EQ(0, v1Response->get_executors().completed_executors_size());
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
    ASSERT_EQ(0, v1Response->get_executors().executors_size());
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
  EXPECT_NE(0u, offers->size());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
    ASSERT_EQ(0, v1Response->get_tasks().pending_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().queued_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().launched_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().terminated_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().completed_tasks_size());
  }

  driver.launchTasks(offer.id(), {task});

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
    ASSERT_EQ(0, v1Response->get_tasks().pending_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().queued_tasks_size());
    ASSERT_EQ(1, v1Response->get_tasks().launched_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().terminated_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().completed_tasks_size());
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
    ASSERT_EQ(0, v1Response->get_tasks().pending_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().queued_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().launched_tasks_size());
    ASSERT_EQ(0, v1Response->get_tasks().terminated_tasks_size());
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
  ASSERT_EQ(flags.hostname,
            v1Response->get_agent().agent_info().hostname());
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
  EXPECT_NE(0u, offers->size());
  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");
  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
    ASSERT_EQ(0u, getState.get_frameworks().frameworks_size());
    ASSERT_EQ(0u, getState.get_tasks().launched_tasks_size());
    ASSERT_EQ(0u, getState.get_executors().executors_size());
  }

  driver.launchTasks(offer.id(), {task});

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
    ASSERT_EQ(1u, getState.get_frameworks().frameworks_size());
    ASSERT_EQ(0u, getState.get_frameworks().completed_frameworks_size());
    ASSERT_EQ(1u, getState.get_tasks().launched_tasks_size());
    ASSERT_EQ(0u, getState.get_tasks().completed_tasks_size());
    ASSERT_EQ(1u, getState.get_executors().executors_size());
    ASSERT_EQ(0u, getState.get_executors().completed_executors_size());
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
    ASSERT_EQ(0u, getState.get_frameworks().frameworks_size());
    ASSERT_EQ(1u, getState.get_frameworks().completed_frameworks_size());
    ASSERT_EQ(0u, getState.get_tasks().launched_tasks_size());
    ASSERT_EQ(1u, getState.get_tasks().completed_tasks_size());
    ASSERT_EQ(0u, getState.get_executors().executors_size());
    ASSERT_EQ(1u, getState.get_executors().completed_executors_size());
  }
}


TEST_P(AgentAPITest, NestedContainerWaitNotFound)
{
  ContentType contentType = GetParam();

  Clock::pause();

  StandaloneMasterDetector detector;
  MockContainerizer mockContainerizer;

  EXPECT_CALL(mockContainerizer, recover(_))
    .WillOnce(Return(Future<Nothing>(Nothing())));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &mockContainerizer);

  ASSERT_SOME(slave);

  // Wait for the agent to finish recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  // Expect a 404 for waiting on unknown containers.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::WAIT_NESTED_CONTAINER);

    v1::ContainerID unknownContainerId;
    unknownContainerId.set_value(UUID::random().toString());
    unknownContainerId.mutable_parent()->set_value(UUID::random().toString());

    call.mutable_wait_nested_container()->mutable_container_id()
      ->CopyFrom(unknownContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
  }

  // The destructor of `cluster::Slave` will try to clean up any
  // remaining containers by inspecting the result of `containers()`.
  EXPECT_CALL(mockContainerizer, containers())
    .WillRepeatedly(Return(hashset<ContainerID>()));
}


TEST_P(AgentAPITest, NestedContainerKillNotFound)
{
  ContentType contentType = GetParam();

  Clock::pause();

  StandaloneMasterDetector detector;
  MockContainerizer mockContainerizer;

  EXPECT_CALL(mockContainerizer, recover(_))
    .WillOnce(Return(Future<Nothing>(Nothing())));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &mockContainerizer);

  ASSERT_SOME(slave);

  // Wait for the agent to finish recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  // Expect a 404 for killing unknown containers.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::KILL_NESTED_CONTAINER);

    v1::ContainerID unknownContainerId;
    unknownContainerId.set_value(UUID::random().toString());
    unknownContainerId.mutable_parent()->set_value(UUID::random().toString());

    call.mutable_kill_nested_container()->mutable_container_id()
      ->CopyFrom(unknownContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::NotFound().status, response);
  }

  // The destructor of `cluster::Slave` will try to clean up any
  // remaining containers by inspecting the result of `containers()`.
  EXPECT_CALL(mockContainerizer, containers())
    .WillRepeatedly(Return(hashset<ContainerID>()));
}


// When containerizer returns false from launching a nested
// container, it is considered a bad request (e.g. image
// type is not supported).
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, NestedContainerLaunchFalse)
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

  // Try to launch an "unsupported" container.
  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  {
    // Return false here to indicate "unsupported".
    EXPECT_CALL(containerizer, launch(_, _, _, _, _, _))
      .WillOnce(Return(Future<bool>(false)));

    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, NestedContainerLaunch)
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

  // Launch a nested container and wait for it to finish.
  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
  containerId.mutable_parent()->set_value(containerIds->begin()->value());

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  Future<v1::agent::Response> wait;

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::WAIT_NESTED_CONTAINER);

    call.mutable_wait_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    wait = post(slave.get()->pid, call, contentType);

    Clock::settle();

    EXPECT_TRUE(wait.isPending());
  }

  // Now kill the nested container.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::KILL_NESTED_CONTAINER);

    call.mutable_kill_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(wait);
  ASSERT_EQ(v1::agent::Response::WAIT_NESTED_CONTAINER, wait->type());

  // The test containerizer sets exit status to 0 when destroyed.
  EXPECT_EQ(0, wait->wait_nested_container().exit_status());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, TwoLevelNestedContainerLaunch)
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

  // Launch a two level nested parent/child container and then wait for them to
  // finish.
  v1::ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());
  parentContainerId.mutable_parent()->set_value(containerIds->begin()->value());

  // Launch the parent container.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(parentContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Launch the child container.
  v1::ContainerID childContainerId;
  childContainerId.set_value(UUID::random().toString());
  childContainerId.mutable_parent()->CopyFrom(parentContainerId);

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(childContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Wait for the parent container.
  Future<v1::agent::Response> waitParent;

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::WAIT_NESTED_CONTAINER);

    call.mutable_wait_nested_container()->mutable_container_id()
      ->CopyFrom(parentContainerId);

    waitParent = post(slave.get()->pid, call, contentType);

    Clock::settle();

    EXPECT_TRUE(waitParent.isPending());
  }

  // Wait for the child container.
  Future<v1::agent::Response> waitChild;

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::WAIT_NESTED_CONTAINER);

    call.mutable_wait_nested_container()->mutable_container_id()
      ->CopyFrom(childContainerId);

    waitChild = post(slave.get()->pid, call, contentType);

    Clock::settle();

    EXPECT_TRUE(waitChild.isPending());
  }

  // Kill the child container.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::KILL_NESTED_CONTAINER);

    call.mutable_kill_nested_container()->mutable_container_id()
      ->CopyFrom(childContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(waitChild);
  ASSERT_EQ(v1::agent::Response::WAIT_NESTED_CONTAINER, waitChild->type());

  // The test containerizer sets exit status to 0 when destroyed.
  EXPECT_EQ(0, waitChild->wait_nested_container().exit_status());

  // The parent container should still be running.
  EXPECT_TRUE(waitParent.isPending());

  // Kill the parent container.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::KILL_NESTED_CONTAINER);

    call.mutable_kill_nested_container()->mutable_container_id()
      ->CopyFrom(parentContainerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, call),
      stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  AWAIT_READY(waitParent);
  ASSERT_EQ(v1::agent::Response::WAIT_NESTED_CONTAINER, waitParent->type());

  // The test containerizer sets exit status to 0 when destroyed.
  EXPECT_EQ(0, waitParent->wait_nested_container().exit_status());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
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
  containerId.set_value(UUID::random().toString());
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


// This test verifies that launching a nested container session results
// in stdout and stderr being streamed correctly.
TEST_P_TEMP_DISABLED_ON_WINDOWS(AgentAPITest, LaunchNestedContainerSession)
{
  ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Launch a nested container session that runs a command
  // that writes something to stdout and stderr and exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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
  Fetcher fetcher;

  Try<MesosContainerizer*> mesosContainerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(mesosContainerizer);

  Owned<slave::Containerizer> containerizer(mesosContainerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  {
    // Default principal is not allowed to launch nested container sessions.
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
      flags.acls.get()
        .add_launch_nested_container_sessions_under_parent_with_user();
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
  EXPECT_NE(0u, offers->size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Attempt to launch a nested container which does nothing.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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
  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Launch a nested container session that runs a command
  // that writes something to stdout and stderr and exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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
  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Launch a nested container session that runs `cat` so that it never exits.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_NE(0u, offers->size());

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

  ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
      serialize, messageContentType, lambda::_1));

  EXPECT_CALL(containerizer, attach(_))
    .WillOnce(Return(process::Failure("Unsupported")));

  Future<http::Response> response = http::post(
    slave.get()->pid,
    "api/v1",
    headers,
    encoder.encode(call),
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
  {
    mesos::ACL::AttachContainerInput* acl =
      flags.acls->add_attach_containers_input();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Launch a nested container session which runs a shell.

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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

    ::recordio::Encoder<v1::agent::Call> encoder(
        lambda::bind(serialize, messageContentType, lambda::_1));

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers[MESSAGE_CONTENT_TYPE] = stringify(messageContentType);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      encoder.encode(call),
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

    ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
        serialize, messageContentType, lambda::_1));

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        encoder.encode(call),
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

    ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
        serialize, messageContentType, lambda::_1));

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        encoder.encode(call),
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

    ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
        serialize, ContentType::PROTOBUF, lambda::_1));

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        encoder.encode(call),
        stringify(ContentType::RECORDIO));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::BadRequest().status, response);
  }

  // Unsupported 'Message-Content-Type' media type for a streaming request.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    call.mutable_attach_container_input()->set_type(
        v1::agent::Call::AttachContainerInput::CONTAINER_ID);

    ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
        serialize, ContentType::PROTOBUF, lambda::_1));

    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers[MESSAGE_CONTENT_TYPE] = "unsupported/media-type";

    Future<http::Response> response = http::post(
        slave.get()->pid,
        "api/v1",
        headers,
        encoder.encode(call),
        stringify(ContentType::RECORDIO));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::UnsupportedMediaType().status,
                                    response);
  }

  // Unsupported 'Message-Accept' media type for a streaming response.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

    v1::ContainerID containerId;
    containerId.set_value(UUID::random().toString());

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
    containerId.set_value(UUID::random().toString());

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
  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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
  containerId.set_value(UUID::random().toString());
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

  ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
      serialize, messageContentType, lambda::_1));

  // Prepare the data that needs to be streamed to the entrypoint
  // of the container.

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
    attach->mutable_container_id()->CopyFrom(containerId);

    writer.write(encoder.encode(call));
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

    writer.write(encoder.encode(call));
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

    writer.write(encoder.encode(call));
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
  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_EQ(1u, containerIds->size());

  v1::ContainerID containerId;
  containerId.set_value(UUID::random().toString());
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

  ::recordio::Encoder<v1::agent::Call> encoder(lambda::bind(
      serialize, messageContentType, lambda::_1));

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_INPUT);

    v1::agent::Call::AttachContainerInput* attach =
      call.mutable_attach_container_input();

    attach->set_type(v1::agent::Call::AttachContainerInput::CONTAINER_ID);
    attach->mutable_container_id()->CopyFrom(containerId);

    writer.write(encoder.encode(call));
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

    writer.write(encoder.encode(call));
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

    writer.write(encoder.encode(call));
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
