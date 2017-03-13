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
#include <vector>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include <process/clock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "master/detector/standalone.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"

namespace http = process::http;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;

using process::http::Forbidden;
using process::http::OK;
using process::http::Response;

using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

template <typename T>
class SlaveAuthorizerTest : public MesosTest {};


typedef ::testing::Types<
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    tests::Module<Authorizer, TestLocalAuthorizer>,
#endif // __WINDOWS__
    LocalAuthorizer> AuthorizerTypes;


TYPED_TEST_CASE(SlaveAuthorizerTest, AuthorizerTypes);


// This test verifies that authorization based endpoint filtering
// works correctly on the /state endpoint.
// Both default users are allowed to to view high level frameworks, but only
// one is allowed to view the tasks.
TYPED_TEST(SlaveAuthorizerTest, FilterStateEndpoint)
{
  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see executors running under any user.
    ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see tasks running under any user.
    ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Register framework with user "bar".
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.set_user("bar");

  // Create an executor with user "bar".
  ExecutorInfo executor = createExecutorInfo("test-executor", "sleep 2");
  executor.mutable_command()->set_user("bar");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
      this->StartSlave(detector.get(), &containerizer, authorizer.get());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    ASSERT_TRUE(framework.values["executors"].is<JSON::Array>());

    JSON::Array executors = framework.values["executors"].as<JSON::Array>();
    EXPECT_EQ(1u, executors.values.size());

    JSON::Object executor = executors.values.front().as<JSON::Object>();
    EXPECT_EQ(1u, executor.values["tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the executor.
  {
    Future<Response> response = http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();
    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_TRUE(framework.values["executors"].as<JSON::Array>().values.empty());
    }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TYPED_TEST(SlaveAuthorizerTest, ViewFlags)
{
  ACLs acls;

  {
    // Default principal can see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_flags()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal cannot see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_flags()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  StandaloneMasterDetector detector;

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> agent =
    this->StartSlave(&detector, authorizer.get());

  ASSERT_SOME(agent);

  AWAIT_READY(recover);

  // Ensure that the slave has finished recovery.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // The default principal should be able to access the flags.
  {
    Future<Response> response = http::get(
        agent.get()->pid,
        "flags",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response->body;

    response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response->body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
    EXPECT_TRUE(1u <= state.values["flags"].as<JSON::Object>().values.size());
  }

  // The second default principal should not have access to the
  // /flags endpoint and get a filtered view of the /state one.
  {
    Future<Response> response = http::get(
        agent.get()->pid,
        "flags",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
        << response->body;

    response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response->body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    EXPECT_TRUE(state.values.find("flags") == state.values.end());
  }
}


// Parameterized fixture for agent-specific authorization tests. The
// path of the tested endpoint is passed as the only parameter.
class SlaveEndpointTest:
    public MesosTest,
    public ::testing::WithParamInterface<string> {};


// The tests are parameterized by the endpoint being queried.
//
// TODO(bbannier): Once agent endpoint handlers use more than just
// `GET_ENDPOINT_WITH_PATH`, we should consider parameterizing
// `SlaveEndpointTest` by the authorization action as well.
INSTANTIATE_TEST_CASE_P(
    Endpoint,
    SlaveEndpointTest,
    ::testing::Values(
        "monitor/statistics",
        "monitor/statistics.json",
        "containers"));


// Tests that an agent endpoint handler forms
// correct queries against the authorizer.
TEST_P(SlaveEndpointTest, AuthorizedRequest)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  MockAuthorizer mockAuthorizer;

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, &mockAuthorizer);
  ASSERT_SOME(agent);

  AWAIT_READY(recover);

  // Ensure that the slave has finished recovery.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  Future<authorization::Request> request;
  EXPECT_CALL(mockAuthorizer, authorized(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(true)));

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(request);

  const string principal = DEFAULT_CREDENTIAL.principal();
  EXPECT_EQ(principal, request->subject().value());

  // TODO(bbannier): Once agent endpoint handlers use more than just
  // `GET_ENDPOINT_WITH_PATH` we should factor out the request method
  // and expected authorization action and parameterize
  // `SlaveEndpointTest` on that as well in addition to the endpoint.
  EXPECT_EQ(authorization::GET_ENDPOINT_WITH_PATH, request->action());

  EXPECT_EQ("/" + endpoint, request->object().value());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;
}


// Tests that unauthorized requests for an agent endpoint are properly rejected.
TEST_P(SlaveEndpointTest, UnauthorizedRequest)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  MockAuthorizer mockAuthorizer;

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, &mockAuthorizer);
  ASSERT_SOME(agent);

  AWAIT_READY(recover);

  // Ensure that the slave has finished recovery.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_CALL(mockAuthorizer, authorized(_))
    .WillOnce(Return(false));

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
    << response->body;
}


// Tests that requests for an agent endpoint
// always succeed if the authorizer is absent.
TEST_P(SlaveEndpointTest, NoAuthorizer)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, CreateSlaveFlags());
  ASSERT_SOME(agent);

  AWAIT_READY(recover);

  // Ensure that the slave has finished recovery.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
