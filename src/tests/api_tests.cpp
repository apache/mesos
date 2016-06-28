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

#include "internal/evolve.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::internal::recordio::Reader;

using mesos::internal::slave::Slave;

using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;

using process::http::Accepted;
using process::http::OK;
using process::http::Pipe;
using process::http::Response;

using recordio::Decoder;

using testing::_;
using testing::AtMost;
using testing::DoAll;
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
    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return process::http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType))
      .then([contentType](const Response& response)
            -> Future<v1::master::Response> {
        if (response.status != OK().status) {
          return Failure("Unexpected response status " + response.status);
        }
        return deserialize<v1::master::Response>(contentType, response.body);
      });
  }

  // Helper for evolving a type by serializing/parsing when the types
  // have not changed across versions.
  template <typename T>
  static T evolve(const google::protobuf::Message& message)
  {
    T t;

    string data;

    // NOTE: We need to use 'SerializePartialToString' instead of
    // 'SerializeToString' because some required fields might not be set
    // and we don't want an exception to get thrown.
    CHECK(message.SerializePartialToString(&data))
      << "Failed to serialize " << message.GetTypeName()
      << " while evolving to " << t.GetTypeName();

    // NOTE: We need to use 'ParsePartialFromString' instead of
    // 'ParsePartialFromString' because some required fields might not
    // be set and we don't want an exception to get thrown.
    CHECK(t.ParsePartialFromString(data))
      << "Failed to parse " << t.GetTypeName()
      << " while evolving from " << message.GetTypeName();

    return t;
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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_AGENTS, v1Response.get().type());
  ASSERT_EQ(v1Response.get().get_agents().agents_size(), 1);

  const v1::master::Response::GetAgents::Agent& v1Agent =
      v1Response.get().get_agents().agents(0);

  ASSERT_EQ("host", v1Agent.agent_info().hostname());
  ASSERT_EQ(agent.get()->pid, v1Agent.pid());
  ASSERT_TRUE(v1Agent.active());
  ASSERT_EQ("1.0.0", v1Agent.version());
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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_FLAGS, v1Response.get().type());
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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_FRAMEWORKS, v1Response.get().type());

  v1::master::Response::GetFrameworks frameworks =
      v1Response.get().get_frameworks();

  ASSERT_EQ(1, frameworks.frameworks_size());
  ASSERT_EQ("default", frameworks.frameworks(0).framework_info().name());
  ASSERT_EQ("*", frameworks.frameworks(0).framework_info().role());
  ASSERT_FALSE(frameworks.frameworks(0).framework_info().checkpoint());
  ASSERT_TRUE(frameworks.frameworks(0).active());
  ASSERT_TRUE(frameworks.frameworks(0).connected());

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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_HEALTH, v1Response.get().type());
  ASSERT_TRUE(v1Response.get().get_health().healthy());
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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_VERSION, v1Response.get().type());

  ASSERT_EQ(MESOS_VERSION,
            v1Response.get().get_version().version_info().version());
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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_METRICS, v1Response.get().type());

  hashmap<string, double> metrics;

  foreach (const v1::Metric& metric,
           v1Response.get().get_metrics().metrics()) {
    ASSERT_TRUE(metric.has_value());
    metrics[metric.name()] = metric.value();
  }

  // Verifies that the response metrics is not empty.
  ASSERT_LE(0, metrics.size());
}


// This tests v1 API GetTasks when no task is present.
TEST_P(MasterAPITest, GetTasksNoRunningTask)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.mutable_get_tasks();
  v1Call.set_type(v1::master::Call::GET_TASKS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_TASKS, v1Response.get().type());
  ASSERT_EQ(0, v1Response.get().get_tasks().tasks_size());
}


// This tests v1 API GetTasks with 1 running task.
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

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());
  EXPECT_TRUE(status.get().has_executor_id());
  EXPECT_EQ(exec.id, status.get().executor_id());

  v1::master::Call v1Call;
  v1Call.mutable_get_tasks();
  v1Call.set_type(v1::master::Call::GET_TASKS);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_TASKS, v1Response.get().type());
  ASSERT_EQ(1, v1Response.get().get_tasks().tasks_size());
  ASSERT_EQ(v1::TaskState::TASK_RUNNING,
            v1Response.get().get_tasks().tasks(0).state());
  ASSERT_EQ("test", v1Response.get().get_tasks().tasks(0).name());
  ASSERT_EQ("1", v1Response.get().get_tasks().tasks(0).task_id().value());

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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_LOGGING_LEVEL, v1Response.get().type());
  ASSERT_LE(0, FLAGS_v);
  ASSERT_EQ(
      v1Response.get().get_logging_level().level(),
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
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<Nothing> v1Response = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  ASSERT_EQ(v1::Resources::parse(slaveFlags.resources.get()).get(),
            v1Response->get_roles().roles(1).resources());

  driver.stop();
  driver.join();
}


TEST_P(MasterAPITest, GetLeadingMaster)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_LEADING_MASTER);

  ContentType contentType = GetParam();

  Future<v1::master::Response> v1Response =
    post(master.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_LEADING_MASTER, v1Response->type());
  ASSERT_EQ(master.get()->getMasterInfo().ip(),
            v1Response->get_leading_master().master_info().ip());
}


// This test verifies that an operator can reserve available resources through
// the `RESERVE_RESOURCES` call.
TEST_P(MasterAPITest, ReserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo .set_role("role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

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

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::RESERVE_RESOURCES);

  v1::master::Call::ReserveResources* reserveResources =
    v1Call.mutable_reserve_resources();

  reserveResources->mutable_agent_id()->CopyFrom(
    internal::evolve(slaveId.get()));

  reserveResources->mutable_resources()->CopyFrom(
    internal::evolve<v1::Resource>(
      static_cast<const RepeatedPtrField<Resource>&>(dynamicallyReserved)));

  ContentType contentType = GetParam();

  Future<Response> response = process::http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  driver.stop();
  driver.join();
}


// This test verifies that an operator can unreserve reserved resources through
// the `UNRESERVE_RESOURCES` call.
TEST_P(MasterAPITest, UnreserveResources)
{
  TestAllocator<> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  Future<SlaveID> slaveId;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<0>(&slaveId)));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo .set_role("role");

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(DEFAULT_CREDENTIAL.principal()));

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

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::RESERVE_RESOURCES);

  v1::master::Call::ReserveResources* reserveResources =
    v1Call.mutable_reserve_resources();

  reserveResources->mutable_agent_id()->CopyFrom(
    internal::evolve(slaveId.get()));

  reserveResources->mutable_resources()->CopyFrom(
    internal::evolve<v1::Resource>(
      static_cast<const RepeatedPtrField<Resource>&>(dynamicallyReserved)));

  ContentType contentType = GetParam();

  Future<Response> reserveResponse = process::http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, reserveResponse);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(dynamicallyReserved));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Expect an offer to be rescinded!
  EXPECT_CALL(sched, offerRescinded(_, _));

  // Unreserve the resources.
  v1Call.set_type(v1::master::Call::UNRESERVE_RESOURCES);

  v1::master::Call::UnreserveResources* unreserveResources =
    v1Call.mutable_unreserve_resources();

  unreserveResources->mutable_agent_id()->CopyFrom(
    internal::evolve(slaveId.get()));

  unreserveResources->mutable_resources()->CopyFrom(
    internal::evolve<v1::Resource>(
      static_cast<const RepeatedPtrField<Resource>&>(dynamicallyReserved)));

  Future<Response> unreserveResponse = process::http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, unreserveResponse);

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(unreserved));

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
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  // Generate `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  machine1.set_hostname("Machine1");
  machine2.set_ip("0.0.0.2");

  // Try to schedule maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, createUnavailability(Clock::now()))});
  v1::maintenance::Schedule v1Schedule =
    evolve<v1::maintenance::Schedule>(schedule);

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  Future<Nothing> v1UpdateScheduleResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  ASSERT_TRUE(v1GetScheduleResponse.get().IsInitialized());
  ASSERT_EQ(
      v1::master::Response::GET_MAINTENANCE_SCHEDULE,
      v1GetScheduleResponse.get().type());

  // Verify maintenance schedule matches the expectation.
  v1::maintenance::Schedule respSchedule =
    v1GetScheduleResponse.get().get_maintenance_schedule().schedule();
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
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  // Generate `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  machine1.set_hostname("Machine1");
  machine2.set_ip("0.0.0.2");

  // Try to schedule maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, createUnavailability(Clock::now()))});
  v1::maintenance::Schedule v1Schedule =
    evolve<v1::maintenance::Schedule>(schedule);

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  Future<Nothing> v1UpdateScheduleResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  ASSERT_TRUE(v1GetStatusResponse.get().IsInitialized());
  ASSERT_EQ(
      v1::master::Response::GET_MAINTENANCE_STATUS,
      v1GetStatusResponse.get().type());

  // Verify maintenance status matches the expectation.
  v1::maintenance::ClusterStatus status =
    v1GetStatusResponse.get().get_maintenance_status().status();
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
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
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
  v1::maintenance::Schedule v1Schedule =
    evolve<v1::maintenance::Schedule>(schedule);

  v1::master::Call v1UpdateScheduleCall;
  v1UpdateScheduleCall.set_type(v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE);
  v1::master::Call_UpdateMaintenanceSchedule* maintenanceSchedule =
    v1UpdateScheduleCall.mutable_update_maintenance_schedule();
  maintenanceSchedule->mutable_schedule()->CopyFrom(v1Schedule);

  Future<Nothing> v1UpdateScheduleResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1UpdateScheduleCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  startMaintenance->add_machines()->CopyFrom(evolve<v1::MachineID>(machine3));

  Future<Nothing> v1StartMaintenanceResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1StartMaintenanceCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  stopMaintenance->add_machines()->CopyFrom(evolve<v1::MachineID>(machine3));

  Future<Nothing> v1StopMaintenanceResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1StopMaintenanceCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  ASSERT_TRUE(v1GetScheduleResponse.get().IsInitialized());
  ASSERT_EQ(
      v1::master::Response::GET_MAINTENANCE_SCHEDULE,
      v1GetScheduleResponse.get().type());

  // Check that only one maintenance window remains.
  v1::maintenance::Schedule respSchedule =
    v1GetScheduleResponse.get().get_maintenance_schedule().schedule();
  ASSERT_EQ(1, respSchedule.windows().size());
  ASSERT_EQ(2, respSchedule.windows(0).machine_ids().size());
  ASSERT_EQ("Machine1", respSchedule.windows(0).machine_ids(0).hostname());
  ASSERT_EQ("0.0.0.2", respSchedule.windows(0).machine_ids(1).ip());
}


// This test tries to verify that a client subscribed to the 'api/v1'
// endpoint is able to receive `TASK_ADDED`/`TASK_UPDATED` events.
TEST_P(MasterAPITest, Subscribe)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::SUBSCRIBE);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  ContentType contentType = GetParam();
  headers["Accept"] = stringify(contentType);

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response.get().type);
  ASSERT_SOME(response->reader);

  Pipe::Reader reader = response->reader.get();

  auto deserializer =
    lambda::bind(deserialize<v1::master::Event>, contentType, lambda::_1);

  Reader<v1::master::Event> decoder(
      Decoder<v1::master::Event>(deserializer), reader);

  Future<Result<v1::master::Event>> event = decoder.read();

  EXPECT_TRUE(event.isPending());

  // Launch a task using the scheduler. This should result in a `TASK_ADDED`
  // event when the task is launched followed by a `TASK_UPDATED` event after
  // the task transitions to running state.
  auto scheduler = std::make_shared<MockV1HTTPScheduler>();
  auto executor = std::make_shared<MockV1HTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::TestV1Mesos mesos(master.get()->pid, contentType, scheduler);

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
    subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);

  TaskInfo task = createTask(internal::devolve(offer), "", executorId);

  EXPECT_CALL(*scheduler, update(_, _))
    .Times(2);

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(executor::SendSubscribe(
        frameworkId, internal::evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(executor::SendUpdateFromTask(
        frameworkId, internal::evolve(executorId), v1::TASK_RUNNING));

  EXPECT_CALL(*executor, acknowledged(_, _));

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::ACCEPT);

    call.mutable_framework_id()->CopyFrom(frameworkId);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);

    operation->mutable_launch()->add_task_infos()->CopyFrom(
        internal::evolve(task));

    mesos.send(call);
  }

  AWAIT_READY(event);

  ASSERT_EQ(v1::master::Event::TASK_ADDED, event.get().get().type());
  ASSERT_EQ(internal::evolve(task.task_id()),
            event.get().get().task_added().task().task_id());

  event = decoder.read();

  AWAIT_READY(event);

  ASSERT_EQ(v1::master::Event::TASK_UPDATED, event.get().get().type());
  ASSERT_EQ(v1::TASK_RUNNING, event.get().get().task_updated().state());

  event = decoder.read();

  // After we advance the clock, the status update manager would retry the
  // `TASK_RUNNING` update. Since, the state of the task is not changed, this
  // should not result in another `TASK_UPDATED` event.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

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
    Future<Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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
  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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
    Future<Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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
    Future<Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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
  SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

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

  createVolumes->add_volumes()->CopyFrom(evolve<v1::Resource>(volume));
  createVolumes->mutable_agent_id()->CopyFrom(evolve<v1::AgentID>(slaveId));

  ContentType contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<Nothing> v1CreateVolumesResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != Accepted().status) {
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

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(volume));

  Resources taskResources = Resources::parse(
      "disk:256",
      frameworkInfo.role()).get();

  TaskInfo taskInfo = createTask(
      offer.slave_id(),
      taskResources,
      "sleep 1",
      DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_FINISHED, status.get().state());

  // Destroy the persistent volume.
  v1::master::Call v1DestroyVolumesCall;
  v1DestroyVolumesCall.set_type(v1::master::Call::DESTROY_VOLUMES);
  v1::master::Call_DestroyVolumes* destroyVolumes =
    v1DestroyVolumesCall.mutable_destroy_volumes();

  destroyVolumes->mutable_agent_id()->CopyFrom(evolve<v1::AgentID>(slaveId));
  destroyVolumes->add_volumes()->CopyFrom(evolve<v1::Resource>(volume));

  Future<Nothing> v1DestroyVolumesResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1DestroyVolumesCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != Accepted().status) {
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

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<Nothing> updateResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, updateCall),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return process::http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType))
      .then([contentType](const Response& response)
            -> Future<v1::agent::Response> {
        if (response.status != OK().status) {
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


TEST_P(AgentAPITest, GetFlags)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_FLAGS);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_FLAGS, v1Response.get().type());
}


TEST_P(AgentAPITest, GetHealth)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_HEALTH);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_HEALTH, v1Response.get().type());
  ASSERT_TRUE(v1Response.get().get_health().healthy());
}


TEST_P(AgentAPITest, GetVersion)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_VERSION);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_VERSION, v1Response.get().type());

  ASSERT_EQ(MESOS_VERSION,
            v1Response.get().get_version().version_info().version());
}


TEST_P(AgentAPITest, GetMetrics)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

  Duration timeout = Seconds(5);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_METRICS);
  v1Call.mutable_get_metrics()->mutable_timeout()->set_nanoseconds(
      timeout.ns());

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_METRICS, v1Response.get().type());

  hashmap<string, double> metrics;

  foreach (const v1::Metric& metric,
           v1Response.get().get_metrics().metrics()) {
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

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::GET_LOGGING_LEVEL);

  ContentType contentType = GetParam();

  Future<v1::agent::Response> v1Response =
    post(slave.get()->pid, v1Call, contentType);

  AWAIT_READY(v1Response);
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_LOGGING_LEVEL, v1Response.get().type());
  ASSERT_LE(0, FLAGS_v);
  ASSERT_EQ(
      v1Response.get().get_logging_level().level(),
      static_cast<uint32_t>(FLAGS_v));
}


// Test the logging level toggle and revert after specific toggle duration.
TEST_P(AgentAPITest, SetLoggingLevel)
{
  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> slave = this->StartSlave(&detector);
  ASSERT_SOME(slave);

  // Wait until the agent has finished recovery.
  AWAIT_READY(__recover);

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
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<Nothing> v1Response = process::http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType))
    .then([contentType](const Response& response) -> Future<Nothing> {
      if (response.status != OK().status) {
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
  EXPECT_NE(0u, offers.get().size());

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
    ASSERT_TRUE(v1Response.get().IsInitialized());
    ASSERT_EQ(v1::agent::Response::GET_CONTAINERS, v1Response.get().type());
    ASSERT_EQ(0, v1Response.get().get_containers().containers_size());
  }

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

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
  ASSERT_TRUE(v1Response.get().IsInitialized());
  ASSERT_EQ(v1::agent::Response::GET_CONTAINERS, v1Response.get().type());
  ASSERT_EQ(1, v1Response.get().get_containers().containers_size());
  ASSERT_EQ("192.168.1.20",
            v1Response.get().get_containers().containers(0).container_status()
              .network_infos(0).ip_addresses(0).ip_address());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
