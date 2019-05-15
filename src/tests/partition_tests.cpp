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

#include <vector>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"
#include "master/registry_operations.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;
using process::Time;
using process::UPID;

using process::http::OK;
using process::http::Response;

using std::vector;

using testing::_;
using testing::AllOf;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class PartitionTest : public MesosTest {};


// This test checks that a scheduler gets a slave lost
// message for a partitioned slave.
TEST_F(PartitionTest, PartitionedSlave)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  Clock::pause();

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now advance through the PINGs.
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

  AWAIT_READY(slaveLost);

  slave.get()->terminate();
  slave->reset();

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that if a `RunTaskMessage` dropped en route to
// an agent which later becomes unreachable, the task is removed correctly
// from the master's unreachable task records when the agent reregisters.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    PartitionTest,
    ReregisterPartitionedAgentWithEnrouteTask)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  // Start a scheduler. The scheduler has the PARTITION_AWARE
  // capability, so we expect its tasks to continue running when the
  // partitioned agent reregisters.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
  FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  // Drop the RunTaskMessage.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), master.get()->pid, slave.get()->pid);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(runTaskMessage);

  const SlaveID& slaveId = offer.slave_id();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  // We expect to get a `slaveLost` callback, even though this
  // scheduler is partition-aware.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());

  AWAIT_READY(slaveLost);

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(0, stats.values["master/tasks_lost"]);
    EXPECT_EQ(1, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  }

  // Check the master's "/state" endpoint. There should be a single
  // unreachable agent. The "tasks" and "completed_tasks" fields
  // should be empty; there should be a single unreachable task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_EQ(0, parse->values["activated_slaves"].as<JSON::Number>());
    EXPECT_EQ(0, parse->values["deactivated_slaves"].as<JSON::Number>());
    EXPECT_EQ(1, parse->values["unreachable_slaves"].as<JSON::Number>());

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(runningTasks.values.empty());

    JSON::Array unreachableTasks =
    framework.values["unreachable_tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, unreachableTasks.values.size());

    JSON::Object unreachableTask =
      unreachableTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
    task.task_id(), unreachableTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_UNREACHABLE",
        unreachableTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/tasks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array tasks = parse->values["tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, tasks.values.size());

    JSON::Object jsonTask = tasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
    task.task_id(), jsonTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_UNREACHABLE",
        jsonTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(0, framework.values["TASK_LOST"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_RUNNING"].as<JSON::Number>());
    EXPECT_EQ(1, framework.values["TASK_UNREACHABLE"].as<JSON::Number>());
  }

  // We now complete the partition on the slave side as well. We
  // simulate a master loss event, which would normally happen during
  // a network partition. The slave should then reregister with the
  // master.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  Clock::resume();

  // Check the master's "/state" endpoint. The "unreachable_tasks" and
  // "completed_tasks" fields should be empty.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array completedTasks =
    framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());

    JSON::Array unreachableTasks =
    framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_TRUE(unreachableTasks.values.empty());

    JSON::Array tasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(tasks.values.empty());
  }

  // Check the master's "/tasks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array tasks = parse->values["tasks"].as<JSON::Array>();

    ASSERT_TRUE(tasks.values.empty());
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(0, framework.values["TASK_LOST"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_UNREACHABLE"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_RUNNING"].as<JSON::Number>());
  }

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(0, stats.values["master/tasks_running"]);
    EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
    EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  }

  driver.stop();
  driver.join();
}


// This test checks that a slave can reregister with the master after
// a partition, and that PARTITION_AWARE tasks running on the slave
// continue to run.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, ReregisterSlavePartitionAware)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  // Start a scheduler. The scheduler has the PARTITION_AWARE
  // capability, so we expect its tasks to continue running when the
  // partitioned agent reregisters.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  AWAIT_READY(statusUpdateAck2);

  const SlaveID& slaveId = startingStatus->slave_id();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  // We expect to get a `slaveLost` callback, even though this
  // scheduler is partition-aware.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());

  AWAIT_READY(slaveLost);

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(0, stats.values["master/tasks_lost"]);
    EXPECT_EQ(1, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  }

  // Check the master's "/state" endpoint. There should be a single
  // unreachable agent. The "tasks" and "completed_tasks" fields
  // should be empty; there should be a single unreachable task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_EQ(0, parse->values["activated_slaves"].as<JSON::Number>());
    EXPECT_EQ(0, parse->values["deactivated_slaves"].as<JSON::Number>());
    EXPECT_EQ(1, parse->values["unreachable_slaves"].as<JSON::Number>());

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(runningTasks.values.empty());

    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, unreachableTasks.values.size());

    JSON::Object unreachableTask =
      unreachableTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), unreachableTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_UNREACHABLE",
        unreachableTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/tasks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array tasks = parse->values["tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, tasks.values.size());

    JSON::Object jsonTask = tasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), jsonTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_UNREACHABLE",
        jsonTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(0, framework.values["TASK_LOST"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_RUNNING"].as<JSON::Number>());
    EXPECT_EQ(1, framework.values["TASK_UNREACHABLE"].as<JSON::Number>());
  }

  // We now complete the partition on the slave side as well. We
  // simulate a master loss event, which would normally happen during
  // a network partition. The slave should then reregister with the
  // master.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<TaskStatus> runningAgainStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningAgainStatus));

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  AWAIT_READY(runningAgainStatus);
  EXPECT_EQ(TASK_RUNNING, runningAgainStatus->state());
  EXPECT_EQ(task.task_id(), runningAgainStatus->task_id());

  Clock::resume();

  // Check the master's "/state" endpoint. The "unreachable_tasks" and
  // "completed_tasks" fields should be empty; there should be a
  // single running task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());

    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_TRUE(unreachableTasks.values.empty());

    JSON::Array tasks = framework.values["tasks"].as<JSON::Array>();

    JSON::Object activeTask = tasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), activeTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_RUNNING", activeTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/tasks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array tasks = parse->values["tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, tasks.values.size());

    JSON::Object jsonTask = tasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), jsonTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_RUNNING",
        jsonTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(0, framework.values["TASK_LOST"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_UNREACHABLE"].as<JSON::Number>());
    EXPECT_EQ(1, framework.values["TASK_RUNNING"].as<JSON::Number>());
  }

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(1, stats.values["master/tasks_running"]);
    EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
    EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  }

  driver.stop();
  driver.join();
}


// This test checks that a slave can reregister with the master after
// a partition, and that non-PARTITION_AWARE tasks are still running on the
// slave.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, ReregisterSlaveNotPartitionAware)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  // Start a scheduler. The scheduler is not PARTITION_AWARE, so we
  // expect its tasks to be shutdown when the partitioned agent
  // reregisters.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID& slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unhealthy. We then advance the clock -- this shouldn't
  // do anything, but it ensures that the `unreachable_time` we check
  // below is computed at the right time.
  TimeInfo partitionTime = protobuf::getCurrentTime();

  Clock::advance(Milliseconds(100));

  // The scheduler should see TASK_LOST because it is not
  // PARTITION_AWARE.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());
  EXPECT_EQ(task.task_id(), lostStatus->task_id());
  EXPECT_EQ(slaveId, lostStatus->slave_id());
  EXPECT_EQ(partitionTime, lostStatus->unreachable_time());

  AWAIT_READY(slaveLost);

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(1, stats.values["master/tasks_lost"]);
    EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
    EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  }

  // Check the master's "/state" endpoint. The "tasks" and
  // "unreachable_tasks" fields should be empty; there should be a
  // single completed task (we report LOST tasks as "completed" for
  // backward compatibility).
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(runningTasks.values.empty());

    // Although the task state based metrics above show the task in
    // master/tasks_lost, the state endpoint lists the task in the
    // "unreachable_tasks" section because it is indeed unreachable
    // and shouldn't be considered "completed". This difference is
    // unfortunate and should be addressed in MESOS-8405.
    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_FALSE(unreachableTasks.values.empty());

    JSON::Object unreachableTask =
      unreachableTasks.values.front().as<JSON::Object>();

    // The unreachable task is in its backwards-compatible state.
    EXPECT_EQ(
        task.task_id(), unreachableTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_LOST",
        unreachableTask.values["state"].as<JSON::String>().value);

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());
  }

  // We now complete the partition on the slave side as well. We
  // simulate a master loss event, which would normally happen during
  // a network partition. The slave should then reregister with the
  // master.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<TaskStatus> runningAgainStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningAgainStatus));

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  AWAIT_READY(runningAgainStatus);
  EXPECT_EQ(TASK_RUNNING, runningAgainStatus->state());
  EXPECT_EQ(task.task_id(), runningAgainStatus->task_id());

  Clock::resume();

  // Check the master's "/state" endpoint. The "tasks" and
  // "unreachable_tasks" fields should be empty and there should be
  // no completed tasks.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_EQ(1u, runningTasks.values.size());

    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_TRUE(unreachableTasks.values.empty());

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_TRUE(completedTasks.values.empty());
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(1, framework.values["TASK_RUNNING"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_UNREACHABLE"].as<JSON::Number>());
    EXPECT_EQ(0, framework.values["TASK_LOST"].as<JSON::Number>());
  }

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(1, stats.values["master/tasks_lost"]);
    EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(0, stats.values["master/tasks_killed"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
    EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
    EXPECT_EQ(1, stats.values["master/slave_removals"]);
    EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
    EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  }

  driver.stop();
  driver.join();
}


// This tests that an agent can reregister with the master after a
// partition in which the master has failed over while the agent was
// partitioned. We use one agent and two schedulers; one scheduler
// enables the PARTITION_AWARE capability, while the other does
// not. Both tasks should survive the reregistration of the partitioned
// agent: we allow the non-partition-aware task to continue running for
// backward compatibility with the "non-strict" Mesos 1.0 behavior.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    PartitionTest,
    PartitionedSlaveReregistrationMasterFailover)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:1024";

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Connect the first scheduler (not PARTITION_AWARE).
  MockScheduler sched1;
  TestingMesosSchedulerDriver driver1(&sched1, &detector);

  EXPECT_CALL(sched1, registered(&driver1, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resources taskResources = Resources::parse("cpus:1;mem:512").get();
  taskResources.allocate(DEFAULT_FRAMEWORK_INFO.roles(0));

  EXPECT_TRUE(Resources(offer.resources()).contains(taskResources));

  // Launch `task1` using `sched1`.
  TaskInfo task1 = createTask(offer.slave_id(), taskResources, "sleep 60");

  Future<TaskStatus> startingStatus1;
  Future<TaskStatus> runningStatus1;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&startingStatus1))
    .WillOnce(FutureArg<1>(&runningStatus1));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offer.id(), {task1});

  AWAIT_READY(runningStatus1);
  EXPECT_EQ(TASK_RUNNING, runningStatus1->state());
  EXPECT_EQ(task1.task_id(), runningStatus1->task_id());

  const SlaveID& slaveId = runningStatus1->slave_id();

  AWAIT_READY(statusUpdateAck1);

  // Connect the second scheduler (PARTITION_AWARE).
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched2;
  TestingMesosSchedulerDriver driver2(&sched2, &detector, frameworkInfo2);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver2.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(taskResources));

  // Launch the second task.
  TaskInfo task2 = createTask(offer.slave_id(), taskResources, "sleep 60");

  Future<TaskStatus> startingStatus2;
  Future<TaskStatus> runningStatus2;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&startingStatus2))
    .WillOnce(FutureArg<1>(&runningStatus2));

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver2.launchTasks(offer.id(), {task2});

  AWAIT_READY(runningStatus2);
  EXPECT_EQ(TASK_RUNNING, runningStatus2->state());
  EXPECT_EQ(task2.task_id(), runningStatus2->task_id());
  EXPECT_EQ(slaveId, runningStatus2->slave_id());

  AWAIT_READY(statusUpdateAck2);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  // Note that we expect to get `slaveLost` callbacks in both
  // schedulers, regardless of PARTITION_AWARE.
  Future<Nothing> slaveLost1;
  EXPECT_CALL(sched1, slaveLost(&driver1, _))
    .WillOnce(FutureSatisfy(&slaveLost1));

  Future<Nothing> slaveLost2;
  EXPECT_CALL(sched2, slaveLost(&driver2, _))
    .WillOnce(FutureSatisfy(&slaveLost2));

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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unhealthy. We then advance the clock -- this shouldn't
  // do anything, but it ensures that the `unreachable_time` we check
  // below is computed at the right time.
  TimeInfo partitionTime = protobuf::getCurrentTime();

  Clock::advance(Milliseconds(100));

  // `sched1` should see TASK_LOST.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());
  EXPECT_EQ(task1.task_id(), lostStatus->task_id());
  EXPECT_EQ(slaveId, lostStatus->slave_id());
  EXPECT_EQ(partitionTime, lostStatus->unreachable_time());

  // `sched2` should see TASK_UNREACHABLE.
  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task2.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());
  EXPECT_EQ(partitionTime, unreachableStatus->unreachable_time());

  // The master should notify both schedulers that the slave was lost.
  AWAIT_READY(slaveLost1);
  AWAIT_READY(slaveLost2);

  EXPECT_CALL(sched1, disconnected(&driver1));
  EXPECT_CALL(sched2, disconnected(&driver2));

  // Simulate master failover.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes recovering the registry.
  Clock::settle();

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<Nothing> registered1;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureSatisfy(&registered1));

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  Future<TaskStatus> runningAgainStatus1;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&runningAgainStatus1));

  Future<TaskStatus> runningAgainStatus2;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&runningAgainStatus2));

  // Simulate a new master detected event to the slave and the schedulers.
  detector.appoint(master.get()->pid);

  // Wait for both schedulers to reregister.
  AWAIT_READY(registered1);
  AWAIT_READY(registered2);

  // Register the agent after the schedulers to make sure they receive the
  // status updates sent by the master.
  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  AWAIT_READY(runningAgainStatus1);
  EXPECT_EQ(TASK_RUNNING, runningAgainStatus1->state());

  AWAIT_READY(runningAgainStatus2);
  EXPECT_EQ(TASK_RUNNING, runningAgainStatus2->state());

  Clock::resume();

  {
    JSON::Object stats = Metrics();
    EXPECT_EQ(2, stats.values["master/tasks_running"]);
    EXPECT_EQ(0, stats.values["master/tasks_lost"]);
    EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
    EXPECT_EQ(0, stats.values["master/slave_removals"]);
  }

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This test causes a slave to be partitioned while it is running a
// task for a PARTITION_AWARE framework. The scheduler is shutdown
// before the partition heals; the task should be shutdown after the
// agent reregisters.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, PartitionedSlaveOrphanedTask)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(frameworkId);

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Launch `task` using `sched`.
  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID& slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck2);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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
  Clock::settle();

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());
  EXPECT_TRUE(unreachableStatus->has_unreachable_time());

  AWAIT_READY(slaveLost);

  // Disconnect the scheduler. The default `failover_timeout` is 0, so
  // the framework's tasks should be shutdown when the slave reregisters.
  driver.stop();
  driver.join();

  // Before the agent reregisters, check how `task` is displayed by
  // the master's "/state" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    EXPECT_TRUE(parse->values["frameworks"].as<JSON::Array>().values.empty());
    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks.values.size());

    JSON::Object framework =
      completedFrameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        frameworkId.get(),
        framework.values["id"].as<JSON::String>().value);

    EXPECT_TRUE(framework.values["tasks"].as<JSON::Array>().values.empty());
    EXPECT_TRUE(
        framework.values["unreachable_tasks"].as<JSON::Array>().values.empty());

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedTasks.values.size());

    JSON::Object completedTask =
      completedTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(),
        completedTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_KILLED",
        completedTask.values["state"].as<JSON::String>().value);
  }

  // Cause the slave to reregister with the master; the master should
  // send a `ShutdownFrameworkMessage` to the slave.
  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<ShutdownFrameworkMessage> shutdownFramework = FUTURE_PROTOBUF(
      ShutdownFrameworkMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);
  AWAIT_READY(shutdownFramework);

  Clock::resume();

  // After the agent reregisters, check how `task` is displayed by
  // the master's "/state" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    EXPECT_TRUE(parse->values["frameworks"].as<JSON::Array>().values.empty());
    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks.values.size());

    JSON::Object framework =
      completedFrameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        frameworkId.get(),
        framework.values["id"].as<JSON::String>().value);

    EXPECT_TRUE(framework.values["tasks"].as<JSON::Array>().values.empty());
    EXPECT_TRUE(
        framework.values["unreachable_tasks"].as<JSON::Array>().values.empty());

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedTasks.values.size());

    JSON::Object completedTask =
      completedTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(),
        completedTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_KILLED",
        completedTask.values["state"].as<JSON::String>().value);
  }

  // Check the master's "/state-summary" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    EXPECT_TRUE( frameworks.values.empty());
  }

  // Also check the master's "/tasks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array tasks = parse->values["tasks"].as<JSON::Array>();

    ASSERT_EQ(1u, tasks.values.size());

    JSON::Object jsonTask = tasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), jsonTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_KILLED",
        jsonTask.values["state"].as<JSON::String>().value);
  }
}


// This test checks that the master handles a slave that becomes
// partitioned while running a task that belongs to a disconnected
// framework.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, DisconnectedFramework)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_failover_timeout(Weeks(2).secs());

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);

  ASSERT_FALSE(offers1->empty());
  Offer offer = offers1.get()[0];

  // Launch `task` using `sched1`.
  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID& slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck2);

  // Shutdown the master.
  master->reset();

  // Stop the framework while the master is down.
  driver1.stop();
  driver1.join();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  // Notify the slave about the new master and wait for it to
  // reregister. The task on the agent should continue running,
  // because the master has failed over.
  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregistered);

  Clock::pause();

  // Cause the slave to be partitioned from the master.
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
  Clock::settle();

  // Failover to a new scheduler instance using the same framework ID.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.mutable_id()->MergeFrom(frameworkId.get());
  frameworkInfo2.set_failover_timeout(frameworkInfo1.failover_timeout());

  // TODO(neilc): We need to introduce an inner scope here to ensure
  // the `MesosSchedulerDriver` gets destroyed before we fetch
  // metrics, to workaround MESOS-6231.
  {
    MockScheduler sched2;
    MesosSchedulerDriver driver2(
        &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered2;
    EXPECT_CALL(sched2, registered(&driver2, _, _))
      .WillOnce(FutureSatisfy(&registered2));

    driver2.start();

    AWAIT_READY(registered2);

    // Perform explicit reconciliation.
    TaskStatus status;
    status.mutable_task_id()->CopyFrom(task.task_id());
    status.mutable_slave_id()->CopyFrom(slaveId);
    status.set_state(TASK_STAGING); // Dummy value.

    Future<TaskStatus> reconcileUpdate;
    EXPECT_CALL(sched2, statusUpdate(&driver2, _))
      .WillOnce(FutureArg<1>(&reconcileUpdate));

    driver2.reconcileTasks({status});

    AWAIT_READY(reconcileUpdate);
    EXPECT_EQ(TASK_LOST, reconcileUpdate->state());
    EXPECT_EQ(TaskStatus::REASON_RECONCILIATION,
              reconcileUpdate->reason());
    EXPECT_TRUE(reconcileUpdate->has_unreachable_time());

    Clock::resume();

    driver2.stop();
    driver2.join();
  }

  JSON::Object stats = Metrics();
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
}


// This test checks that when a registered slave reregisters with the
// master (e.g., because of a spurious Zk leader flap at the slave),
// the master does not kill any tasks on the slave, even if those
// tasks are not PARTITION_AWARE.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, SpuriousSlaveReregistration)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  // The framework should not be PARTITION_AWARE, since tasks started
  // by PARTITION_AWARE frameworks will never be killed on reregistration.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  ASSERT_FALSE(protobuf::frameworkHasCapability(
      frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(frameworkId);

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Launch `task` using `sched`.
  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID& slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck2);

  // Simulate a master loss event at the slave and then cause the
  // slave to reregister with the master. From the master's
  // perspective, the slave reregisters while it was still both
  // connected and registered.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  // Perform explicit reconciliation. The task should still be running.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_RUNNING, reconcileUpdate->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate->reason());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks how Mesos behaves when a slave is removed because
// it fails health checks, and then the slave sends a status update
// (because it does not realize that it is partitioned from the
// master's POV). In prior Mesos versions, the master would shutdown
// the slave in this situation. In Mesos >= 1.1, the master will drop
// the status update; the slave will eventually try to reregister.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, PartitionedSlaveStatusUpdates)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Drop both PINGs from master to slave and PONGs from slave to
  // master. Note that we don't match on the master / slave PIDs
  // because it's actually the `SlaveObserver` process that sends pings
  // and receives pongs.
  DROP_PROTOBUFS(PingSlaveMessage(), _, _);
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now, induce a partition of the slave by having the master timeout
  // the slave. The master will remove the slave; the slave will also
  // realize that it hasn't seen any pings from the master and try to
  // reregister. We don't want to let the slave reregister yet, so we
  // drop the first message in the reregistration protocol, which is
  // AuthenticateMessage since agent auth is enabled.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  for (size_t i = 0; i < masterFlags.max_agent_ping_timeouts; i++) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  // Slave will try to authenticate for reregistration; message dropped.
  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(authenticateMessage);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);

  // At this point, the slave still thinks it's registered, so we
  // simulate a status update coming from the slave.
  TaskID taskId1;
  taskId1.set_value("task_id1");

  const StatusUpdate& update1 = protobuf::createStatusUpdate(
      frameworkId.get(),
      slaveId,
      taskId1,
      TASK_RUNNING,
      TaskStatus::SOURCE_SLAVE,
      id::UUID::random());

  StatusUpdateMessage message1;
  message1.mutable_update()->CopyFrom(update1);
  message1.set_pid(stringify(slave.get()->pid));

  // The scheduler should not receive the status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  process::post(master.get()->pid, message1);
  Clock::settle();

  // Advance the clock so that the slaves notices that it still hasn't
  // seen any pings from the master, which will cause it to try to
  // reregister again. This time reregistration should succeed.
  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  for (size_t i = 0; i < masterFlags.max_agent_ping_timeouts; i++) {
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  AWAIT_READY(slaveReregistered);

  // Since the slave has reregistered, a status update from the slave
  // should now be forwarded to the scheduler.
  Future<StatusUpdateMessage> statusUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), master.get()->pid, _);

  TaskID taskId2;
  taskId2.set_value("task_id2");

  const StatusUpdate& update2 = protobuf::createStatusUpdate(
      frameworkId.get(),
      slaveId,
      taskId2,
      TASK_RUNNING,
      TaskStatus::SOURCE_SLAVE,
      id::UUID::random());

  StatusUpdateMessage message2;
  message2.mutable_update()->CopyFrom(update2);
  message2.set_pid(stringify(slave.get()->pid));

  process::post(master.get()->pid, message2);

  AWAIT_READY(statusUpdate);
  EXPECT_EQ(taskId2, statusUpdate->update().status().task_id());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks how Mesos behaves when a slave is removed, and
// then the slave sends an ExitedExecutorMessage (because it does not
// realize it is partitioned from the master's POV). In prior Mesos
// versions, the master would shutdown the slave in this situation. In
// Mesos >= 1.1, the master will drop the message; the slave will
// eventually try to reregister.
TEST_F(PartitionTest, PartitionedSlaveExitedExecutor)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));\

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task. This allows us to have the slave send an
  // ExitedExecutorMessage.
  TaskInfo task = createTask(offers.get()[0], "sleep 60", DEFAULT_EXECUTOR_ID);

  // Set up the expectations for launching the task.
  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the status updates from the slave.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get()->pid);

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
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

  // The master will notify the framework of the lost task.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);

  EXPECT_CALL(sched, executorLost(&driver, _, _, _))
    .Times(0);

  // Induce an ExitedExecutorMessage from the slave.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  // The master will drop the ExitedExecutorMessage. We do not
  // currently support reliable delivery of ExitedExecutorMessages, so
  // the message will not be delivered if/when the slave reregisters.
  //
  // TODO(neilc): Update this test to check for reliable delivery once
  // MESOS-4308 is fixed.
  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that when a non-partition-aware task finishes
// while an agent is partitioned, that task is displayed correctly at
// the master after the partition heals.
TEST_F(PartitionTest, TaskCompletedOnPartitionedAgent)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Start a non-partition-aware scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);
  const SlaveID& slaveId = offer.slave_id();

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<Nothing> runningAtScheduler;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&runningAtScheduler));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(slaveId);
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(execTask);

  {
    TaskStatus runningStatus;
    runningStatus.mutable_task_id()->MergeFrom(execTask->task_id());
    runningStatus.set_state(TASK_RUNNING);

    execDriver->sendStatusUpdate(runningStatus);
  }

  AWAIT_READY(runningAtScheduler);
  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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
  Clock::settle();

  // The scheduler should see TASK_LOST because it is not
  // PARTITION_AWARE.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());
  EXPECT_EQ(task.task_id(), lostStatus->task_id());
  EXPECT_EQ(slaveId, lostStatus->slave_id());
  EXPECT_TRUE(lostStatus->has_unreachable_time());

  AWAIT_READY(slaveLost);

  // Have the executor inform the slave that the task has finished.
  {
    TaskStatus finishedStatus;
    finishedStatus.mutable_task_id()->MergeFrom(execTask->task_id());
    finishedStatus.set_state(TASK_FINISHED);

    execDriver->sendStatusUpdate(finishedStatus);
  }

  // Ensure the agent processes the task finished update.
  Clock::settle();

  // Cause the slave to reregister with the master. Because the
  // framework is not partition-aware, this results in shutting down
  // the executor on the slave. The enqueued TASK_FINISHED update
  // should also be propagated to the scheduler.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<TaskStatus> finishedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&finishedStatus))
    .WillRepeatedly(Return()); // The agent may resend status updates.

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  AWAIT_READY(finishedStatus);
  EXPECT_EQ(TASK_FINISHED, finishedStatus->state());
  EXPECT_EQ(task.task_id(), finishedStatus->task_id());
  EXPECT_EQ(slaveId, finishedStatus->slave_id());

  // Perform explicit reconciliation. The task should not be running
  // (TASK_LOST) because the framework is not PARTITION_AWARE.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_LOST, reconcileUpdate->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate->reason());
  EXPECT_FALSE(reconcileUpdate->has_unreachable_time());

  Clock::resume();

  // Check the master's "/state" endpoint. The "tasks" and
  // "unreachable_tasks" fields should be empty; there should be a
  // single completed task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(runningTasks.values.empty());

    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_TRUE(unreachableTasks.values.empty());

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    JSON::Object completedTask =
      completedTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), completedTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_FINISHED",
        completedTask.values["state"].as<JSON::String>().value);
  }

  driver.stop();
  driver.join();
}


// This test verifies that when a partition-aware task finishes while
// an agent is partitioned, that task is displayed correctly at the
// master after the partition heals.
TEST_F(PartitionTest, PartitionAwareTaskCompletedOnPartitionedAgent)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Start a partition-aware scheduler.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);
  const SlaveID& slaveId = offer.slave_id();

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<Nothing> runningAtScheduler;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&runningAtScheduler));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(slaveId);
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(execTask);

  {
    Future<Nothing> ___statusUpdate =
      FUTURE_DISPATCH(slave.get()->pid, &Slave::___statusUpdate);

    TaskStatus runningStatus;
    runningStatus.mutable_task_id()->MergeFrom(execTask->task_id());
    runningStatus.set_state(TASK_RUNNING);

    execDriver->sendStatusUpdate(runningStatus);

    AWAIT_READY(___statusUpdate);
  }

  AWAIT_READY(runningAtScheduler);
  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

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
  Clock::settle();

  // The scheduler should see TASK_UNREACHABLE because it is
  // PARTITION_AWARE.
  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());
  EXPECT_TRUE(unreachableStatus->has_unreachable_time());

  AWAIT_READY(slaveLost);

  // Have the executor inform the slave that the task has finished.
  {
    Future<Nothing> ___statusUpdate =
      FUTURE_DISPATCH(slave.get()->pid, &Slave::___statusUpdate);

    TaskStatus finishedStatus;
    finishedStatus.mutable_task_id()->MergeFrom(execTask->task_id());
    finishedStatus.set_state(TASK_FINISHED);

    execDriver->sendStatusUpdate(finishedStatus);

    AWAIT_READY(___statusUpdate);
  }

  // Cause the slave to reregister with the master. Because the
  // framework is partition-aware, this should not result in shutting
  // down the executor on the slave. The enqueued TASK_FINISHED update
  // should also be propagated to the scheduler.
  //
  // The master sends the current framework PID to the slave via
  // `UpdateFrameworkMessage`; the slave then resends any pending
  // status updates. This would make the test non-deterministic; since
  // the framework PID is unchanged, drop the `UpdateFrameworkMessage`.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  Future<UpdateFrameworkMessage> frameworkUpdate = DROP_PROTOBUF(
      UpdateFrameworkMessage(), master.get()->pid, slave.get()->pid);

  // The `finishedStatusFromMaster` status update is sent as
  // part of agent's re-registration with the master.
  // The second status update `finishedStatusFromAgent` here
  // is sent by the agent's status update manager after it
  // re-regsiters.
  Future<TaskStatus> finishedStatusFromMaster;
  Future<TaskStatus> finishedStatusFromAgent;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&finishedStatusFromMaster))
    .WillOnce(FutureArg<1>(&finishedStatusFromAgent));

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);

  AWAIT_READY(finishedStatusFromMaster);
  EXPECT_EQ(TASK_FINISHED, finishedStatusFromMaster->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, finishedStatusFromMaster->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REREGISTERED,
      finishedStatusFromMaster->reason());
  EXPECT_EQ(task.task_id(), finishedStatusFromMaster->task_id());

  AWAIT_READY(frameworkUpdate);

  AWAIT_READY(finishedStatusFromAgent);
  EXPECT_EQ(TASK_FINISHED, finishedStatusFromAgent->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, finishedStatusFromAgent->source());
  EXPECT_EQ(task.task_id(), finishedStatusFromAgent->task_id());
  EXPECT_EQ(slaveId, finishedStatusFromAgent->slave_id());

  // Perform explicit reconciliation. The task should not be running.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_GONE, reconcileUpdate->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate->reason());
  EXPECT_FALSE(reconcileUpdate->has_unreachable_time());

  // Check the master's "/state" endpoint. The "tasks" and
  // "unreachable_tasks" fields should be empty; there should be a
  // single completed task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    EXPECT_TRUE(parse->values["orphan_tasks"].as<JSON::Array>().values.empty());

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    JSON::Array runningTasks = framework.values["tasks"].as<JSON::Array>();

    EXPECT_TRUE(runningTasks.values.empty());

    JSON::Array unreachableTasks =
      framework.values["unreachable_tasks"].as<JSON::Array>();

    EXPECT_TRUE(unreachableTasks.values.empty());

    JSON::Array completedTasks =
      framework.values["completed_tasks"].as<JSON::Array>();

    JSON::Object completedTask =
      completedTasks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        task.task_id(), completedTask.values["id"].as<JSON::String>().value);
    EXPECT_EQ(
        "TASK_FINISHED",
        completedTask.values["state"].as<JSON::String>().value);
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that when the master removes a lost agent any
// unacknowledged but terminal tasks on the agent are tracked as
// unreachable but keep their original terminal state.
TEST_F(PartitionTest, AgentWithTerminalTaskPartitioned)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  // We require the completed task to still be tracked on the master when
  // we lose the agent. By disabling scheduler implicit acknowledgements
  // and controlling the acknowledgements manually, we ensure that the
  // master is forced to retain the task state.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  testing::Sequence taskSequence;
  Future<TaskStatus> starting;
  Future<TaskStatus> running;
  Future<TaskStatus> finished;

  TaskInfo task = createTask(offers->at(0), SLEEP_COMMAND(0));

  EXPECT_CALL(
      sched,
      statusUpdate(&driver, AllOf(
          TaskStatusTaskIdEq(task.task_id()),
          TaskStatusStateEq(TASK_STARTING))))
    .InSequence(taskSequence)
    .WillOnce(FutureArg<1>(&starting));

  EXPECT_CALL(
      sched,
      statusUpdate(&driver, AllOf(
          TaskStatusTaskIdEq(task.task_id()),
          TaskStatusStateEq(TASK_RUNNING))))
    .InSequence(taskSequence)
    .WillOnce(FutureArg<1>(&running));

  EXPECT_CALL(
      sched,
      statusUpdate(&driver, AllOf(
          TaskStatusTaskIdEq(task.task_id()),
          TaskStatusStateEq(TASK_FINISHED))))
    .InSequence(taskSequence)
    .WillOnce(FutureArg<1>(&finished))
    // Ignore additional status update delivery attempts.
    .WillRepeatedly(Return());

  Clock::pause();

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(starting);
  driver.acknowledgeStatusUpdate(starting.get());

  AWAIT_READY(running);
  driver.acknowledgeStatusUpdate(running.get());

  // Wait for the task to finish but don't acknowledge the status
  // update. This ensures that the master is still tracking the task
  // when the agent becomes lost.
  AWAIT_READY(finished);

  // When the agent is lost, the master will rescind any offers and we
  // can just ignore that.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(1);

  Future<Nothing> unreachable =
    FUTURE_DISPATCH(_, &master::Master::markUnreachable);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  // Now fast forward through the ping timeouts so that the agent
  // becomes unreachable on the master.
  for (unsigned i = 0; i <= masterFlags.max_agent_ping_timeouts; ++i) {
    Future<PingSlaveMessage> ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
    AWAIT_READY(ping);
  }

  AWAIT_READY(unreachable);

  Clock::settle();
  Clock::resume();

  Future<Response> response = process::http::get(
      master.get()->pid,
      "frameworks",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(json);

  // We should not have any active tasks.
  EXPECT_NONE(json->find<JSON::Number>("frameworks[0].tasks[0]"));

  // The task we launched should be unreachable (since the final status
  // update wasn't acknowledged) but still terminal. Note that these
  // expectations are likely to change as part of MESOS-8405.
  EXPECT_SOME_EQ(
      task.task_id().value(),
      json->find<JSON::String>("frameworks[0].unreachable_tasks[0].id"));
  EXPECT_SOME_EQ(
      "TASK_FINISHED",
      json->find<JSON::String>("frameworks[0].unreachable_tasks[0].state"));

  driver.stop();
  driver.join();
}


// This test checks that the master correctly garbage collects
// information about unreachable agents from the registry using the
// count-based GC criterion.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, RegistryGcByCount)
{
  // Configure GC to only keep the most recent partitioned agent in
  // the unreachable list.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_max_agent_count = 1;

  // Test logic assumes that two agents can be marked unreachable in
  // sequence without triggering GC.
  const Duration health_check_duration =
    masterFlags.agent_ping_timeout * masterFlags.max_agent_ping_timeouts;

  ASSERT_GE(masterFlags.registry_gc_interval, health_check_duration * 2);

  // Pause the clock before starting the master. This ensures that we
  // know precisely when the GC timer will fire.
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags1 = CreateSlaveFlags();
  Owned<MasterDetector> slaveDetector1 = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(slaveDetector1.get(), agentFlags1);
  ASSERT_SOME(slave1);

  Clock::advance(agentFlags1.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage1);
  const SlaveID slaveId1 = slaveRegisteredMessage1->slave_id();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(offers);

  TaskInfo task = createTask(offers->at(0), SLEEP_COMMAND(60));

  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  // Drop RunTaskMessage so that the task stays in TASK_STAGING.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), _, _);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(runTaskMessage);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Future<Nothing> slaveLost1;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost1));

  // Simulate the first slave becoming partitioned from the master.
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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unreachable. We then advance the clock -- this shouldn't
  // do anything, but it ensures that the `unreachable_time` we check
  // below is computed at the right time.
  TimeInfo partitionTime1 = protobuf::getCurrentTime();

  Clock::advance(Milliseconds(100));

  AWAIT_READY(unreachableStatus);

  AWAIT_READY(slaveLost1);

  // Set the expectation before resetting `slave1`.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave1.get()->pid));

  // Shutdown the first slave. This is necessary because we only drop
  // PONG messages; after advancing the clock below, the slave would
  // try to reregister and would succeed. Hence, stop the slave first.
  slave1.get()->terminate();
  slave1->reset();

  // Start another slave.
  slave::Flags agentFlags2 = CreateSlaveFlags();
  Owned<MasterDetector> slaveDetector2 = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(slaveDetector2.get(), agentFlags2);
  ASSERT_SOME(slave2);

  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage2);
  const SlaveID slaveId2 = slaveRegisteredMessage2->slave_id();

  Future<Nothing> slaveLost2;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost2));

  // Simulate the second slave becoming partitioned from the master.
  pings = 0;
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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unreachable. We then advance the clock -- this shouldn't
  // do anything, but it ensures that the `unreachable_time` we check
  // below is computed at the right time.
  TimeInfo partitionTime2 = protobuf::getCurrentTime();

  Clock::advance(Milliseconds(100));

  AWAIT_READY(slaveLost2);

  // Shutdown the second slave. This is necessary because we only drop
  // PONG messages; after advancing the clock below, the slave would
  // try to reregister and would succeed. Hence, stop the slave first.
  slave2.get()->terminate();
  slave2->reset();

  // Do explicit reconciliation for the task launched on `slave1`. GC
  // has not occurred yet (since `registry_gc_interval` has not
  // elapsed since the master was started), so the slave should be in
  // the unreachable list; hence `unreachable_time` should be set on
  // the result of the reconciliation request.
  TaskStatus status1;
  status1.mutable_task_id()->CopyFrom(task.task_id());
  status1.mutable_slave_id()->CopyFrom(slaveId1);
  status1.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status1});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_EQ(partitionTime1, reconcileUpdate1->unreachable_time());

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/tasks_unreachable"]);

  // Advance the clock to cause GC to be performed.
  Clock::advance(masterFlags.registry_gc_interval);
  Clock::settle();

  // Ensure task has been removed from unreachable list once the
  // agent is GC'ed.
  stats = Metrics();
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);

  // Do explicit reconciliation for the task launched on `slave1`.
  // Because the agent has been removed from the unreachable list in
  // the registry, `unreachable_time` should NOT be set.
  TaskStatus status2;
  status2.mutable_task_id()->CopyFrom(task.task_id());
  status2.mutable_slave_id()->CopyFrom(slaveId1);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status2});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_UNKNOWN, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());
  EXPECT_FALSE(reconcileUpdate2->has_unreachable_time());

  // Do explicit reconciliation for a random task ID on the second
  // partitioned slave. Because the agent is still in the unreachable
  // list in the registry, `unreachable_time` should be set.
  TaskStatus status3;
  status3.mutable_task_id()->set_value(id::UUID::random().toString());
  status3.mutable_slave_id()->CopyFrom(slaveId2);
  status3.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate3));

  driver.reconcileTasks({status3});

  AWAIT_READY(reconcileUpdate3);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate3->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate3->reason());
  EXPECT_EQ(partitionTime2, reconcileUpdate3->unreachable_time());

  stats = Metrics();
  EXPECT_EQ(2, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(2, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(2, stats.values["master/slave_removals"]);
  EXPECT_EQ(2, stats.values["master/slave_removals/reason_unhealthy"]);

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test ensures that when using the count-based criterion to
// garbage collect unreachable agents from the registry, the agents
// that were marked unreachable first are the ones that are
// removed. This requires creating a large unreachable list, which
// would be annoying to do by creating slaves and simulating network
// partitions; instead we add agents to the unreachable list by
// directly applying registry operations.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, RegistryGcByCountManySlaves)
{
  // Configure GC to only keep the most recent partitioned agent in
  // the unreachable list.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";
  masterFlags.registry_max_agent_count = 1;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Clock::pause();

  TimeInfo unreachableTime = protobuf::getCurrentTime();

  vector<SlaveID> slaveIDs;
  for (int i = 0; i < 50; i++) {
    SlaveID slaveID;
    slaveID.set_value(id::UUID::random().toString());
    slaveIDs.push_back(slaveID);

    SlaveInfo slaveInfo;
    slaveInfo.set_hostname("localhost");
    slaveInfo.mutable_id()->CopyFrom(slaveID);

    Future<bool> admitApply =
      master.get()->registrar->unmocked_apply(
          Owned<master::RegistryOperation>(
              new master::AdmitSlave(slaveInfo)));

    AWAIT_EXPECT_TRUE(admitApply);

    Future<bool> unreachableApply =
      master.get()->registrar->unmocked_apply(
          Owned<master::RegistryOperation>(
              new master::MarkSlaveUnreachable(slaveInfo, unreachableTime)));

    AWAIT_EXPECT_TRUE(unreachableApply);
  }

  // Restart the master to recover from updated registry.
  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Advance the clock to cause GC to be performed.
  Clock::advance(masterFlags.registry_gc_interval);
  Clock::settle();

  // Start a partition-aware scheduler. We verify GC behavior by doing
  // reconciliation.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // We expect that only the most-recently inserted SlaveID will have
  // survived GC. We also check that the second-most-recent SlaveID
  // has been GC'd.
  SlaveID keptSlaveID = slaveIDs.back();
  SlaveID removedSlaveID = *(slaveIDs.crbegin() + 1);

  TaskStatus status1;
  status1.mutable_task_id()->set_value(id::UUID::random().toString());
  status1.mutable_slave_id()->CopyFrom(keptSlaveID);
  status1.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status1});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_EQ(unreachableTime, reconcileUpdate1->unreachable_time());

  TaskStatus status2;
  status2.mutable_task_id()->set_value(id::UUID::random().toString());
  status2.mutable_slave_id()->CopyFrom(removedSlaveID);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status2});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_UNKNOWN, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());
  EXPECT_FALSE(reconcileUpdate2->has_unreachable_time());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks that the master correctly garbage collects
// information about unreachable agents from the registry using the
// age-based GC criterion. We configure GC to discard agents after 20
// minutes; GC occurs every 15 mins. We test the following schedule:
//
// 1 sec:               slave1 registered
// 76 sec:              slave1 is marked unreachable
// 720 secs (12 mins):  slave2 registers, is marked unreachable
// 900 secs (15 mins):  GC runs, nothing discarded
// 1800 secs (30 mins): GC runs, slave1 is discarded
// 2700 secs (45 mins): GC runs, slave2 is discarded
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, RegistryGcByAge)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_gc_interval = Minutes(15);
  masterFlags.registry_max_agent_age = Minutes(20);

  slave::Flags agentFlags1 = CreateSlaveFlags();
  agentFlags1.registration_backoff_factor = Seconds(1);

  slave::Flags agentFlags2 = CreateSlaveFlags();
  agentFlags2.registration_backoff_factor = Seconds(1);

  // Pause the clock before starting the master. This ensures that we
  // know precisely when the GC timer will fire.
  Clock::pause();

  Time startTime = Clock::now();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> slaveDetector1 = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(slaveDetector1.get(), agentFlags1);
  ASSERT_SOME(slave1);

  // Advance clock to trigger agent registration.
  Clock::advance(agentFlags1.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage1);
  const SlaveID slaveId1 = slaveRegisteredMessage1->slave_id();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Future<Nothing> slaveLost1;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost1));

  // Simulate the first slave becoming partitioned from the master.
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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unreachable.
  TimeInfo partitionTime1 = protobuf::getCurrentTime();

  AWAIT_READY(slaveLost1);

  // Set the expectation before resetting `slave1`.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave1.get()->pid));

  // Shutdown the first slave. This is necessary because we only drop
  // PONG messages; after advancing the clock below, the slave would
  // try to reregister and would succeed. Hence, stop the slave first.
  slave1.get()->terminate();
  slave1->reset();

  // Per the schedule above, we want the second slave to be
  // partitioned after 720 seconds have elapsed, so we advance the
  // clock by 570 seconds (570 + 75 + 75 = 720).
  EXPECT_EQ(Seconds(75) + agentFlags1.registration_backoff_factor,
            Clock::now() - startTime);

  // Advance clock to 720 seconds, minus the time we took to register agent1,
  // and minus the time we will take to register agent 2.
  Clock::advance(Seconds(570) - agentFlags1.registration_backoff_factor -
    agentFlags2.registration_backoff_factor);

  // Start another slave.
  Owned<MasterDetector> slaveDetector2 = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave2 = StartSlave(slaveDetector2.get(),
                                                 agentFlags2);
  ASSERT_SOME(slave2);

  // Advance clock to trigger agent registration.
  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage2);
  const SlaveID slaveId2 = slaveRegisteredMessage2->slave_id();

  Future<Nothing> slaveLost2;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost2));

  // Simulate the second slave becoming partitioned from the master.
  pings = 0;
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
  Clock::settle();

  // Record the time at which we expect the master to have marked the
  // agent as unreachable.
  TimeInfo partitionTime2 = protobuf::getCurrentTime();

  AWAIT_READY(slaveLost2);

  // Shutdown the second slave. This is necessary because we only drop
  // PONG messages; after advancing the clock below, the slave would
  // try to reregister and would succeed. Hence, stop the slave first.
  slave2.get()->terminate();
  slave2->reset();

  EXPECT_EQ(Seconds(720), Clock::now() - startTime);

  // Advance the clock to trigger a GC. The first GC occurs at 900
  // elapsed seconds, so we advance by 180 seconds.
  Clock::advance(Seconds(180));
  Clock::settle();

  // Do explicit reconciliation for random task IDs on both slaves.
  // Since neither slave has exceeded the age-based GC bound, we
  // expect to find both slaves (i.e., `unreachable_time` will be set).
  TaskStatus status1;
  status1.mutable_task_id()->set_value(id::UUID::random().toString());
  status1.mutable_slave_id()->CopyFrom(slaveId1);
  status1.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status1});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_EQ(partitionTime1, reconcileUpdate1->unreachable_time());

  TaskStatus status2;
  status2.mutable_task_id()->set_value(id::UUID::random().toString());
  status2.mutable_slave_id()->CopyFrom(slaveId2);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status2});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());
  EXPECT_EQ(partitionTime2, reconcileUpdate2->unreachable_time());

  // Advance the clock to cause GC to be performed.
  Clock::advance(Minutes(15));
  Clock::settle();

  // Do explicit reconciliation for random task IDs on both slaves.
  // We expect `slave1` to have been garbage collected, but `slave2`
  // should still be present in the registry.
  TaskStatus status3;
  status3.mutable_task_id()->set_value(id::UUID::random().toString());
  status3.mutable_slave_id()->CopyFrom(slaveId1);
  status3.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate3));

  driver.reconcileTasks({status3});

  AWAIT_READY(reconcileUpdate3);
  EXPECT_EQ(TASK_UNKNOWN, reconcileUpdate3->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate3->reason());
  EXPECT_FALSE(reconcileUpdate3->has_unreachable_time());

  TaskStatus status4;
  status4.mutable_task_id()->set_value(id::UUID::random().toString());
  status4.mutable_slave_id()->CopyFrom(slaveId2);
  status4.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate4));

  driver.reconcileTasks({status4});

  AWAIT_READY(reconcileUpdate4);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate4->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate4->reason());
  EXPECT_EQ(partitionTime2, reconcileUpdate4->unreachable_time());

  // Advance the clock to cause GC to be performed.
  Clock::advance(Minutes(15));
  Clock::settle();

  // Do explicit reconciliation for a random task ID on `slave2`. We
  // expect that it has been garbage collected, which means
  // `unreachable_time` will not be set.
  TaskStatus status5;
  status5.mutable_task_id()->set_value(id::UUID::random().toString());
  status5.mutable_slave_id()->CopyFrom(slaveId2);
  status5.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate5;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate5));

  driver.reconcileTasks({status5});

  AWAIT_READY(reconcileUpdate5);
  EXPECT_EQ(TASK_UNKNOWN, reconcileUpdate5->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate5->reason());
  EXPECT_FALSE(reconcileUpdate5->has_unreachable_time());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks what happens when these two operations race:
// garbage collecting some slave IDs from the "unreachable" list in
// the registry and moving a slave from the unreachable list back to
// the admitted list. We add three agents to the unreachable list and
// configure GC to only keep a single agent. Concurrently with GC
// running, we arrange for one of those agents to reregister with the
// master.
TEST_F_TEMP_DISABLED_ON_WINDOWS(PartitionTest, RegistryGcRace)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_max_agent_count = 1;

  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector1 = master.get()->createDetector();
  slave::Flags agentFlags1 = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector1.get(), agentFlags1);
  ASSERT_SOME(slave1);

  // Wait for the slave to register and get the slave id.
  Clock::advance(agentFlags1.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage1);
  SlaveID slaveId1 = slaveRegisteredMessage1->slave_id();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(resourceOffers);

  // Induce a partition of the slave.
  size_t pings1 = 0;
  while (true) {
    AWAIT_READY(ping);
    pings1++;
    if (pings1 == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Future<Nothing> slaveLost1;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost1));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(slaveLost1);

  // `slave1` will try to reregister below when we advance the clock.
  // Prevent this by dropping all future messages from it.
  DROP_MESSAGES(_, slave1.get()->pid, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave1.get()->pid));

  StandaloneMasterDetector detector2(master.get()->pid);
  slave::Flags agentFlags2 = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave2 = StartSlave(&detector2, agentFlags2);
  ASSERT_SOME(slave2);

  // Wait for the slave to register and get the slave id.
  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage2);
  SlaveID slaveId2 = slaveRegisteredMessage2->slave_id();

  // Induce a partition of the slave.
  size_t pings2 = 0;
  while (true) {
    AWAIT_READY(ping);
    pings2++;
    if (pings2 == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Future<Nothing> slaveLost2;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost2));

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(slaveLost2);

  // `slave2` will try to reregister below when we advance the clock.
  // Prevent this by dropping the next `ReregisterSlaveMessage` from it.
  Future<Message> reregisterSlave2 =
    DROP_MESSAGE(Eq(ReregisterSlaveMessage().GetTypeName()),
                 slave2.get()->pid,
                 master.get()->pid);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage3 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, AllOf(
        Not(slave1.get()->pid),
        Not(slave2.get()->pid)));

  Owned<MasterDetector> detector3 = master.get()->createDetector();

  slave::Flags agentFlags3 = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave3 = StartSlave(detector3.get(), agentFlags3);
  ASSERT_SOME(slave3);

  // Wait for the slave to register and get the slave id.
  Clock::advance(agentFlags3.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage3);
  SlaveID slaveId3 = slaveRegisteredMessage3->slave_id();

  // Induce a partition of the slave.
  size_t pings3 = 0;
  while (true) {
    AWAIT_READY(ping);
    pings3++;
    if (pings3 == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Future<Nothing> slaveLost3;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost3));

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(slaveLost3);

  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(reregisterSlave2);

  // `slave3` will try to reregister below when we advance the clock.
  // Prevent this by dropping all future messages from it.
  DROP_MESSAGES(_, slave3.get()->pid, _);

  // Cause `slave2` to reregister with the master. We expect the
  // master to update the registry to mark the slave as reachable; we
  // intercept the registry operation.
  Future<Owned<master::RegistryOperation>> markReachable;
  Promise<bool> markReachableContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&markReachable),
                    Return(markReachableContinue.future())));

  detector2.appoint(master.get()->pid);

  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(markReachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveReachable*>(
          markReachable->get()));

  // Trigger GC. Because GC has been configured to preserve a single
  // unreachable slave (the slave marked unreachable most recently),
  // this should result in attempting to prune `slave1` and `slave2`
  // from the unreachable list. We intercept the registry operation to
  // force the race condition with the reregistration of `slave2`.
  Future<Owned<master::RegistryOperation>> prune;
  Promise<bool> pruneContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&prune),
                    Return(pruneContinue.future())));

  Clock::advance(masterFlags.registry_gc_interval);

  AWAIT_READY(prune);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::Prune*>(
          prune->get()));

  // Apply the registry operation to mark the slave reachable, then
  // pass the result back to the master to allow it to continue. We
  // validate that `slave2` is reregistered successfully.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(),
                    master.get()->pid,
                    slave2.get()->pid);

  Future<bool> applyReachable =
    master.get()->registrar->unmocked_apply(markReachable.get());

  AWAIT_READY(applyReachable);
  markReachableContinue.set(applyReachable.get());

  AWAIT_READY(slaveReregisteredMessage);

  // Apply the registry operation to prune the unreachable list, then
  // pass the result back to the master to allow it to continue.
  Future<bool> applyPrune =
    master.get()->registrar->unmocked_apply(prune.get());

  AWAIT_READY(applyPrune);
  pruneContinue.set(applyPrune.get());

  // We expect that `slave1` has been removed from the unreachable
  // list, `slave2` is registered, and `slave3` is still in the
  // unreachable list. We use reconciliation to verify this.

  TaskStatus status1;
  status1.mutable_task_id()->set_value(id::UUID::random().toString());
  status1.mutable_slave_id()->CopyFrom(slaveId1);
  status1.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status1});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_UNKNOWN, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_FALSE(reconcileUpdate1->has_unreachable_time());

  TaskStatus status2;
  status2.mutable_task_id()->set_value(id::UUID::random().toString());
  status2.mutable_slave_id()->CopyFrom(slaveId2);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status2});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_GONE, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());
  EXPECT_FALSE(reconcileUpdate2->has_unreachable_time());

  TaskStatus status3;
  status3.mutable_task_id()->set_value(id::UUID::random().toString());
  status3.mutable_slave_id()->CopyFrom(slaveId3);
  status3.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate3));

  driver.reconcileTasks({status3});

  AWAIT_READY(reconcileUpdate3);
  EXPECT_EQ(TASK_UNREACHABLE, reconcileUpdate3->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate3->reason());
  EXPECT_TRUE(reconcileUpdate3->has_unreachable_time());

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test checks that the master behaves correctly if a slave fails
// health checks twice. At present, this can only occur if the registry
// operation to mark the slave unreachable takes so long that the
// slave fails an additional health check in the mean time.
TEST_F(PartitionTest, FailHealthChecksTwice)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  Clock::pause();

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now advance through the PINGs.
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

  // The slave observer should dispatch a message to the master. We
  // intercept the next registrar operation; this should be the
  // registry operation marking the slave unreachable.
  Future<Nothing> unreachableDispatch1 =
    FUTURE_DISPATCH(master.get()->pid, &Master::markUnreachable);

  Future<Owned<master::RegistryOperation>> markUnreachable;
  Promise<bool> markUnreachableContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&markUnreachable),
                    Return(markUnreachableContinue.future())));

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(unreachableDispatch1);
  AWAIT_READY(markUnreachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveUnreachable*>(
          markUnreachable->get()));

  // Cause the slave to fail another health check. This is possible
  // because we don't shutdown the SlaveObserver until we have marked
  // the slave as unreachable in the registry. The second health check
  // failure should dispatch to the master but this should NOT result
  // in another registry operation.
  Future<Nothing> unreachableDispatch2 =
    FUTURE_DISPATCH(master.get()->pid, &Master::markUnreachable);

  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(unreachableDispatch2);

  // Apply the registry operation, then pass the result back to the
  // master to allow it to continue.
  Future<bool> applyUnreachable =
    master.get()->registrar->unmocked_apply(markUnreachable.get());

  AWAIT_READY(applyUnreachable);
  markUnreachableContinue.set(applyUnreachable.get());

  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(2, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(2, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);

  driver.stop();
  driver.join();

  Clock::resume();
}


class OneWayPartitionTest : public MesosTest {};


// This test verifies that if the master --> slave socket closes and
// the slave is unaware of it (i.e., one-way network partition), slave
// will reregister with the master.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OneWayPartitionTest, MasterToSlave)
{
  // Pausing the clock ensures that the agent does not attempt to
  // register multiple times (see MESOS-7596 for context).
  Clock::pause();

  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Ensure a ping reaches the slave.
  Future<PingSlaveMessage> ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  AWAIT_READY(ping);
  EXPECT_TRUE(ping->connected());

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::deactivateSlave);

  // Inject a slave exited event at the master causing the master
  // to mark the slave as disconnected. The slave should not notice
  // until it receives the next ping message.
  process::inject::exited(slave.get()->pid, master.get()->pid);

  // Wait until master deactivates the slave.
  AWAIT_READY(deactivateSlave);

  ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Let the slave observer send the next ping.
  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(ping);
  EXPECT_FALSE(ping->connected());

  // Slave should reregister.
  Clock::resume();
  AWAIT_READY(slaveReregisteredMessage);
}


// This test verifies that if master --> framework socket closes and the
// framework is not aware of it (i.e., one way network partition), all
// subsequent calls from the framework after the master has marked it as
// disconnected would result in an error message causing the framework to abort.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OneWayPartitionTest, MasterToScheduler)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  AWAIT_READY(registered);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  // Simulate framework disconnection. This should result in an error message.
  ASSERT_TRUE(process::inject::exited(
      frameworkRegisteredMessage->to, master.get()->pid));

  AWAIT_READY(error);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
