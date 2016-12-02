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

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using process::Future;
using process::Owned;
using process::PID;

using process::http::BadRequest;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

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
        "frameworkId=" + frameworkId.get().value());

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

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
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
      "frameworkId=" + frameworkId.get().value());

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
      "frameworkId=" + frameworkId.get().value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  driver.stop();
  driver.join();
}


// Testing route with deprecated (but still good) ACLs.
// This ACL/test will be removed at the end of the deprecation cycle on 0.27.
TEST_F(TeardownTest, GoodDeprecatedACLs)
{
  // Setup ACLs so that the default principal can teardown the
  // framework.
  ACLs acls;
  mesos::ACL::ShutdownFramework* acl = acls.add_shutdown_frameworks();
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
      "frameworkId=" + frameworkId.get().value());

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
      "frameworkId=" + frameworkId.get().value());

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
      "frameworkId=" + frameworkId.get().value());

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
