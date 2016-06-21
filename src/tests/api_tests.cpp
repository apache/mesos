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

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
