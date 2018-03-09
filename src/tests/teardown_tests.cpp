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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::StandaloneMasterDetector;

using process::Future;
using process::Owned;
using process::PID;

using process::http::BadRequest;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using std::vector;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {


class TeardownTest : public MesosTest {};


// Testing /master/teardown to validate that this endpoint shuts down
// the designated framework or returns an appropriate error.

// Testing route with authorization header and good credentials.
TEST_F(TeardownTest, Success)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "teardown",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        "frameworkId=" + frameworkId->value());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // Check that the framework that was shutdown appears in the
  // "completed_frameworks" list in the master's "/state" endpoint.
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

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    EXPECT_TRUE(frameworks.values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks.values.size());

    JSON::Object completedFramework =
      completedFrameworks.values.front().as<JSON::Object>();

    JSON::String completedFrameworkId =
      completedFramework.values["id"].as<JSON::String>();

    EXPECT_EQ(frameworkId.get(), completedFrameworkId.value);
  }

  driver.stop();
  driver.join();
}


// Testing route with bad credentials.
TEST_F(TeardownTest, BadCredentials)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  Credential badCredential;
  badCredential.set_principal("badPrincipal");
  badCredential.set_secret("badSecret");

  Future<Response> response = process::http::post(
      master.get()->pid,
      "teardown",
      createBasicAuthHeaders(badCredential),
      "frameworkId=" + frameworkId->value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  driver.stop();
  driver.join();
}


// Testing route with good ACLs.
TEST_F(TeardownTest, GoodACLs)
{
  // Setup ACLs so that the default principal can teardown the
  // framework.
  ACLs acls;
  mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_framework_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "teardown",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "frameworkId=" + frameworkId->value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  driver.stop();
  driver.join();
}


// Testing route with bad ACLs.
TEST_F(TeardownTest, BadACLs)
{
  // Setup ACLs so that no principal can teardown the framework.
  ACLs acls;
  mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_framework_principals()->add_values(
      DEFAULT_CREDENTIAL.principal());

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "teardown",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "frameworkId=" + frameworkId->value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  driver.stop();
  driver.join();
}


// Testing route without frameworkId value.
TEST_F(TeardownTest, NoFrameworkId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "teardown",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "");

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  driver.stop();
  driver.join();
}


// Testing route without authorization header.
TEST_F(TeardownTest, NoHeader)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "teardown",
      None(),
      "frameworkId=" + frameworkId->value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  driver.stop();
  driver.join();
}


// This test checks that the teardown operation can be used on a
// framework that has not reregistered after master failover.
TEST_F(TeardownTest, RecoveredFrameworkAfterMasterFailover)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector slaveDetector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector);
  ASSERT_SOME(slave);

  StandaloneMasterDetector schedDetector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 100");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  AWAIT_READY(statusUpdateAck2);

  // Simulate master failover. We leave the scheduler without a master
  // so it does not attempt to reregister.
  EXPECT_CALL(sched, disconnected(&driver));

  schedDetector.appoint(None());
  slaveDetector.appoint(None());

  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slaveDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Teardown the framework, which has not yet reregistered with the
  // new master.
  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "teardown",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        "frameworkId=" + frameworkId->value());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // Check the master's "/state" endpoint to confirm that the
  // "frameworks" and "unregistered_frameworks" keys are empty, and
  // that the framework that was shutdown appears in the
  // "completed_frameworks" key.
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

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    EXPECT_TRUE(frameworks.values.empty());

    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks.values.size());

    JSON::Object completedFramework =
      completedFrameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        frameworkId.get(),
        completedFramework.values["id"].as<JSON::String>().value);
  }

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
