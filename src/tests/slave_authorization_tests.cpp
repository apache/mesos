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

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include <process/authenticator.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/strings.hpp>
#include <stout/try.hpp>

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "authorizer/local/authorizer.hpp"

#include "master/detector/standalone.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/module.hpp"
#include "tests/resources_utils.hpp"

namespace http = process::http;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::Promise;

using process::http::Forbidden;
using process::http::OK;
using process::http::Response;

using process::http::authentication::Principal;

using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

template <typename T>
class SlaveAuthorizerTest : public MesosTest
{
protected:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifndef __WINDOWS__
    // We don't need to actually launch tasks as the specified
    // user, since we are only interested in testing the
    // authorization path.
    flags.switch_user = false;
#endif

    return flags;
  }
};


typedef ::testing::Types<
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    tests::Module<Authorizer, TestLocalAuthorizer>,
#endif // __WINDOWS__
    LocalAuthorizer> AuthorizerTypes;


TYPED_TEST_CASE(SlaveAuthorizerTest, AuthorizerTypes);


// This test verifies that authorization based endpoint filtering
// works correctly on the /state endpoint.
// Both default users are allowed to view high level frameworks, but only
// one is allowed to view the tasks.
// After launching a single task per each framework, one for role "superhero"
// and the other for role "muggle", this test verifies that each of two
// default users can view resource allocations and resource reservations for
// corresponding allowed roles only.
TYPED_TEST(SlaveAuthorizerTest, FilterStateEndpoint)
{
  ACLs acls;

  const string roleSuperhero = "superhero";
  const string roleMuggle = "muggle";

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

  {
    // Default principal can view "superhero" role only.
    ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->add_values(roleSuperhero);

    acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Second default principal can view "muggle" role only.
    ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->add_values(roleMuggle);

    acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Register framework with user "bar" and role "superhero".
  FrameworkInfo frameworkSuperhero = DEFAULT_FRAMEWORK_INFO;
  frameworkSuperhero.set_name("framework-" + roleSuperhero);
  frameworkSuperhero.set_roles(0, roleSuperhero);
  frameworkSuperhero.set_user("bar");

  // Create an executor with user "bar".
  ExecutorInfo executorSuperhero =
    createExecutorInfo("test-executor-" + roleSuperhero, "sleep 2");
  executorSuperhero.mutable_command()->set_user("bar");
  MockExecutor execSuperhero(executorSuperhero.executor_id());

  // Register framework with user "foo" and role "muggle".
  FrameworkInfo frameworkMuggle = DEFAULT_FRAMEWORK_INFO;
  frameworkMuggle.set_name("framework-" + roleMuggle);
  frameworkMuggle.set_principal(DEFAULT_CREDENTIAL_2.principal());
  frameworkMuggle.set_roles(0, roleMuggle);
  frameworkMuggle.set_user("foo");

  // Create an executor with user "foo".
  ExecutorInfo executorMuggle =
    createExecutorInfo("test-executor-" + roleMuggle, "sleep 2");
  executorMuggle.mutable_command()->set_user("foo");
  MockExecutor execMuggle(executorMuggle.executor_id());

  TestContainerizer containerizer(
      {{executorSuperhero.executor_id(), &execSuperhero},
       {executorMuggle.executor_id(), &execMuggle}});

  slave::Flags flags = this->CreateSlaveFlags();
  // Statically reserve resources for each role.
  flags.resources = "cpus(" + roleSuperhero + "):2;" + "cpus(" + roleMuggle +
    "):3;mem(" + roleSuperhero + "):512;" + "mem(" + roleMuggle + "):1024;";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = this->StartSlave(
      detector.get(), &containerizer, authorizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler schedSuperhero;
  MesosSchedulerDriver driverSuperhero(
      &schedSuperhero,
      frameworkSuperhero,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(execSuperhero, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<FrameworkID> frameworkIdSuperhero;
  EXPECT_CALL(schedSuperhero, registered(&driverSuperhero, _, _))
    .WillOnce(FutureArg<1>(&frameworkIdSuperhero));

  Future<vector<Offer>> offersSuperhero;
  EXPECT_CALL(schedSuperhero, resourceOffers(&driverSuperhero, _))
    .WillOnce(FutureArg<1>(&offersSuperhero))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driverSuperhero.start();

  AWAIT_READY(frameworkIdSuperhero);

  AWAIT_READY(offersSuperhero);
  ASSERT_FALSE(offersSuperhero->empty());

  // Define a task which will run on executorSuperhero of frameworkSuperhero.
  TaskInfo taskSuperhero;
  taskSuperhero.set_name("test-" + roleSuperhero);
  taskSuperhero.mutable_task_id()->set_value("1");
  taskSuperhero.mutable_slave_id()->MergeFrom(
      offersSuperhero.get()[0].slave_id());
  taskSuperhero.mutable_resources()->MergeFrom(
      offersSuperhero.get()[0].resources());
  taskSuperhero.mutable_executor()->MergeFrom(executorSuperhero);

  EXPECT_CALL(execSuperhero, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> statusSuperhero;
  EXPECT_CALL(schedSuperhero, statusUpdate(&driverSuperhero, _))
    .WillOnce(FutureArg<1>(&statusSuperhero));

  driverSuperhero.launchTasks(offersSuperhero.get()[0].id(), {taskSuperhero});

  AWAIT_READY(statusSuperhero);
  EXPECT_EQ(TASK_RUNNING, statusSuperhero->state());

  MockScheduler schedMuggle;
  MesosSchedulerDriver driverMuggle(
      &schedMuggle,
      frameworkMuggle,
      master.get()->pid,
      DEFAULT_CREDENTIAL_2);

  EXPECT_CALL(execMuggle, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<FrameworkID> frameworkIdMuggle;
  EXPECT_CALL(schedMuggle, registered(&driverMuggle, _, _))
    .WillOnce(FutureArg<1>(&frameworkIdMuggle));

  Future<vector<Offer>> offersMuggle;
  EXPECT_CALL(schedMuggle, resourceOffers(&driverMuggle, _))
    .WillOnce(FutureArg<1>(&offersMuggle))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driverMuggle.start();

  AWAIT_READY(frameworkIdMuggle);

  AWAIT_READY(offersMuggle);
  ASSERT_FALSE(offersMuggle->empty());

  // Define a task which will run on executorMuggle of frameworkMuggle.
  TaskInfo taskMuggle;
  taskMuggle.set_name("test-" + roleMuggle);
  taskMuggle.mutable_task_id()->set_value("2");
  taskMuggle.mutable_slave_id()->MergeFrom(
      offersMuggle.get()[0].slave_id());
  taskMuggle.mutable_resources()->MergeFrom(
      offersMuggle.get()[0].resources());
  taskMuggle.mutable_executor()->MergeFrom(executorMuggle);

  EXPECT_CALL(execMuggle, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> statusMuggle;
  EXPECT_CALL(schedMuggle, statusUpdate(&driverMuggle, _))
    .WillOnce(FutureArg<1>(&statusMuggle));

  driverMuggle.launchTasks(offersMuggle.get()[0].id(), {taskMuggle});

  AWAIT_READY(statusMuggle);
  ASSERT_EQ(TASK_RUNNING, statusMuggle->state());

  // Retrieve endpoint with the user allowed to view the frameworks.
  // The default user allowed to view role "superhero" only.
  {
    Future<Response> response = http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(2u, frameworks.values.size());

    foreach (const JSON::Value& value, frameworks.values) {
      JSON::Object framework = value.as<JSON::Object>();
      EXPECT_FALSE(framework.values.empty());
      ASSERT_TRUE(framework.values["executors"].is<JSON::Array>());

      JSON::Array executors = framework.values["executors"].as<JSON::Array>();
      EXPECT_EQ(1u, executors.values.size());

      JSON::Object executor = executors.values.front().as<JSON::Object>();
      EXPECT_EQ(1u, executor.values["tasks"].as<JSON::Array>().values.size());
    }

    ASSERT_TRUE(state.values["reserved_resources"].is<JSON::Object>());

    JSON::Object reserved_resources =
      state.values["reserved_resources"].as<JSON::Object>();
    EXPECT_TRUE(reserved_resources.values[roleSuperhero].is<JSON::Object>());
    EXPECT_FALSE(reserved_resources.values[roleMuggle].is<JSON::Object>());

    ASSERT_TRUE(
        state.values["reserved_resources_allocated"].is<JSON::Object>());

    JSON::Object reserved_resources_allocated =
      state.values["reserved_resources_allocated"].as<JSON::Object>();
    EXPECT_TRUE(
        reserved_resources_allocated.values[roleSuperhero].is<JSON::Object>());
    EXPECT_FALSE(
        reserved_resources_allocated.values[roleMuggle].is<JSON::Object>());

    ASSERT_TRUE(state.values["reserved_resources_full"].is<JSON::Object>());

    JSON::Object reserved_resources_full =
      state.values["reserved_resources_full"].as<JSON::Object>();
    EXPECT_TRUE(
        reserved_resources_full.values[roleSuperhero].is<JSON::Array>());
    EXPECT_FALSE(
        reserved_resources_full.values[roleMuggle].is<JSON::Array>());
  }

  // Retrieve endpoint with the user allowed to view the frameworks,
  // but not the executors.
  // The second default user allowed to view role "muggle" only.
  {
    Future<Response> response = http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();
    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(2u, frameworks.values.size());

    foreach (const JSON::Value& value, frameworks.values) {
      JSON::Object framework = value.as<JSON::Object>();
      EXPECT_FALSE(framework.values.empty());
      EXPECT_TRUE(
          framework.values["executors"].as<JSON::Array>().values.empty());
    }

    ASSERT_TRUE(state.values["reserved_resources"].is<JSON::Object>());

    JSON::Object reserved_resources =
      state.values["reserved_resources"].as<JSON::Object>();
    EXPECT_TRUE(reserved_resources.values[roleMuggle].is<JSON::Object>());
    EXPECT_FALSE(reserved_resources.values[roleSuperhero].is<JSON::Object>());

    ASSERT_TRUE(
        state.values["reserved_resources_allocated"].is<JSON::Object>());

    JSON::Object reserved_resources_allocated =
      state.values["reserved_resources_allocated"].as<JSON::Object>();
    EXPECT_TRUE(
        reserved_resources_allocated.values[roleMuggle].is<JSON::Object>());
    EXPECT_FALSE(
        reserved_resources_allocated.values[roleSuperhero].is<JSON::Object>());

    ASSERT_TRUE(state.values["reserved_resources_full"].is<JSON::Object>());

    JSON::Object reserved_resources_full =
      state.values["reserved_resources_full"].as<JSON::Object>();
    EXPECT_TRUE(
        reserved_resources_full.values[roleMuggle].is<JSON::Array>());
    EXPECT_FALSE(
        reserved_resources_full.values[roleSuperhero].is<JSON::Array>());
  }

  EXPECT_CALL(execSuperhero, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(execMuggle, shutdown(_))
    .Times(AtMost(1));

  driverSuperhero.stop();
  driverSuperhero.join();

  driverMuggle.stop();
  driverMuggle.join();
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

    response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    EXPECT_TRUE(state.values.find("flags") == state.values.end());
  }
}


// This test verifies that a task is launched on the agent if the task
// user is authorized based on `run_tasks` ACL configured on the agent
// to only allow whitelisted users to run tasks on the agent.
TYPED_TEST(SlaveAuthorizerTest, AuthorizeRunTaskOnAgent)
{
  // Get the current user.
  Result<string> user = os::user();
  ASSERT_SOME(user) << "Failed to get the current user name"
                    << (user.isError() ? ": " + user.error() : "");

  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  // Start a slave with `bar` and the current user being the only authorized
  // users to launch tasks on the agent.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(ACL::Entity::ANY);
  acl->mutable_users()->add_values("bar");
  acl->mutable_users()->add_values(user.get());

  slave::Flags slaveFlags = this->CreateSlaveFlags();
  slaveFlags.acls = acls;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = this->StartSlave(
      detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Create a framework with user `foo`.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_user("foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Framework is registered since the master admits frameworks of any user.
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Launch the first task with no user, so it defaults to the
  // framework user `foo`.
  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      "sleep 1000");

  // Launch the second task as the current user.
  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      "sleep 1000");
  task2.mutable_command()->set_user(user.get());

  // The first task should fail since the task user `foo` is not an
  // authorized user that can launch a task. However, the second task
  // should succeed.
  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.acceptOffers(
      {offer.id()},
      {LAUNCH({task1, task2})});

  // Wait for TASK_ERROR for 1st task, and TASK_STARTING followed by
  // TASK_RUNNING for 2nd task.
  AWAIT_READY(status0);
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  // Validate both the statuses. Note that the order of receiving the
  // status updates for the 2 tasks is not deterministic, but we know
  // that task2's TASK_RUNNING arrives after TASK_STARTING.
  hashmap<TaskID, TaskStatus> statuses;
  statuses[status0->task_id()] = status0.get();
  statuses[status1->task_id()] = status1.get();
  statuses[status2->task_id()] = status2.get();

  ASSERT_TRUE(statuses.contains(task1.task_id()));
  EXPECT_EQ(TASK_ERROR, statuses.at(task1.task_id()).state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, statuses.at(task1.task_id()).source());
  EXPECT_EQ(TaskStatus::REASON_TASK_UNAUTHORIZED,
            statuses.at(task1.task_id()).reason());

  ASSERT_TRUE(statuses.contains(task2.task_id()));
  EXPECT_EQ(TASK_RUNNING, statuses.at(task2.task_id()).state());

  driver.stop();
  driver.join();
}



// Since executor authentication currently has SSL as a dependency, it does not
// make sense to test executor authorization when Mesos has not been built with
// SSL. In that case, no executor principal will be available on which to
// perform authorization, so we disable the following tests.
#ifdef USE_SSL_SOCKET
class ExecutorAuthorizationTest : public MesosTest {};


// This test verifies that a task group is launched on the agent if the executor
// provides a valid authentication token specifying its own ContainerID.
TEST_F(ExecutorAuthorizationTest, RunTaskGroup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent with permissive ACLs so that a task can be launched.
  ACLs acls;
  acls.set_permissive(true);

  slave::Flags flags = CreateSlaveFlags();
  flags.acls = acls;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.5;mem:32").get(),
      "sleep 1000");

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Resources executorResources =
    allocatedResources(Resources::parse("cpus:0.1;mem:32;disk:32").get(), "*");

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  executor.mutable_resources()->CopyFrom(executorResources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  driver.acceptOffers({offer.id()}, {LAUNCH_GROUP(executor, taskGroup)});

  AWAIT_READY(status);

  ASSERT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_STARTING, status->state());

  driver.stop();
  driver.join();
}


// This test verifies that default executor subscription fails if the executor
// provides a properly-signed authentication token with invalid claims.
TEST_F(ExecutorAuthorizationTest, FailedSubscribe)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent with permissive ACLs so that a task can be launched.
  ACLs acls;
  acls.set_permissive(true);

  Result<Authorizer*> authorizer = Authorizer::create(acls);
  ASSERT_SOME(authorizer);

  slave::Flags flags = CreateSlaveFlags();
  flags.acls = acls;

  Owned<MasterDetector> detector = master.get()->createDetector();

  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Owned<TestContainerizer> containerizer(
      new TestContainerizer(devolve(executorInfo.executor_id()), executor));

  Owned<MockSecretGenerator> mockSecretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      mockSecretGenerator.get(),
      authorizer.get(),
      flags);

  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<v1::executor::Mesos*> executorLib;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLib));

  Owned<JWTSecretGenerator> jwtSecretGenerator(
      new JWTSecretGenerator(DEFAULT_JWT_SECRET_KEY));

  // Create a principal which contains an incorrect ContainerID.
  hashmap<string, string> claims;
  claims["fid"] = frameworkId.value();
  claims["eid"] = v1::DEFAULT_EXECUTOR_ID.value();
  claims["cid"] = id::UUID::random().toString();

  Principal principal(None(), claims);

  // Generate an authentication token which is signed using the correct key,
  // but contains an invalid set of claims.
  Future<Secret> authenticationToken =
    jwtSecretGenerator->generate(principal);

  AWAIT_READY(authenticationToken);

  EXPECT_CALL(*mockSecretGenerator, generate(_))
    .WillOnce(Return(authenticationToken.get()));

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  {
    v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

    v1::TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(taskInfo);

    v1::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(v1::scheduler::Call::ACCEPT);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(executorLib);

  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLib.get()->send(call);
  }

  Future<v1::executor::Event::Error> error;
  EXPECT_CALL(*executor, error(_, _))
    .WillOnce(FutureArg<1>(&error));

  AWAIT_READY(error);
  EXPECT_EQ(
      error->message(),
      "Received unexpected '403 Forbidden' () for SUBSCRIBE");
}


// This test verifies that executor API and operator API calls receive an
// unsuccessful response if the request contains a properly-signed
// authentication token with invalid claims.
TEST_F(ExecutorAuthorizationTest, FailedApiCalls)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent with permissive ACLs so that a task can be launched and the
  // local authorizer's implicit executor authorization will be performed.
  ACLs acls;
  acls.set_permissive(true);

  slave::Flags flags = CreateSlaveFlags();
  flags.acls = acls;

  Owned<MasterDetector> detector = master.get()->createDetector();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Owned<TestContainerizer> containerizer(new TestContainerizer(
      devolve(executorInfo.executor_id()), executor));

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> frameworkSubscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&frameworkSubscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(frameworkSubscribed);
  v1::FrameworkID frameworkId(frameworkSubscribed->framework_id());

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<v1::executor::Mesos*> executorLib;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLib));

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  {
    v1::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(v1::scheduler::Call::ACCEPT);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

    v1::TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(taskInfo);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(executorLib);

  Future<v1::executor::Event::Subscribed> executorSubscribed;
  EXPECT_CALL(*executor, subscribed(_, _))
    .WillOnce(FutureArg<1>(&executorSubscribed));

  Future<Nothing> launchGroup;
  EXPECT_CALL(*executor, launchGroup(_, _))
    .WillOnce(FutureSatisfy(&launchGroup));

  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLib.get()->send(call);
  }

  // Wait for the executor to subscribe. Once it is in the SUBSCRIBED state,
  // the UPDATE and MESSAGE executor calls can be attempted.
  AWAIT_READY(executorSubscribed);
  AWAIT_READY(launchGroup);

  // Create a principal which contains an incorrect ContainerID.
  hashmap<string, string> claims;
  claims["fid"] = frameworkId.value();
  claims["eid"] = v1::DEFAULT_EXECUTOR_ID.value();
  claims["cid"] = id::UUID::random().toString();

  Principal incorrectPrincipal(None(), claims);

  // Generate an authentication token which is signed using the correct key,
  // but contains an invalid set of claims.
  Owned<JWTSecretGenerator> jwtSecretGenerator(
      new JWTSecretGenerator(DEFAULT_JWT_SECRET_KEY));

  Future<Secret> authenticationToken =
    jwtSecretGenerator->generate(incorrectPrincipal);

  AWAIT_READY(authenticationToken);

  v1::ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  containerId.mutable_parent()->CopyFrom(executorSubscribed->container_id());

  http::Headers headers;
  headers["Authorization"] = "Bearer " + authenticationToken->value().data();

  // Since the executor library has already been initialized with a valid
  // authentication token, we use an HTTP helper function to send the
  // executor API and operator API calls with an invalid token.

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

    call.mutable_launch_nested_container_session()->mutable_container_id()
      ->CopyFrom(containerId);
    call.mutable_launch_nested_container_session()->mutable_command()
      ->set_value("sleep 120");

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::WAIT_NESTED_CONTAINER);

    call.mutable_wait_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::KILL_NESTED_CONTAINER);

    call.mutable_kill_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::REMOVE_NESTED_CONTAINER);

    call.mutable_remove_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::ATTACH_CONTAINER_OUTPUT);

    call.mutable_attach_container_output()->mutable_container_id()
      ->CopyFrom(containerId);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
  }

  const string failureMessage =
    "does not contain a 'cid' claim with the correct active ContainerID";

  {
    v1::TaskStatus status;
    status.mutable_task_id()->set_value(id::UUID::random().toString());
    status.set_state(v1::TASK_RUNNING);
    status.set_uuid(id::UUID::random().toBytes());
    status.set_source(v1::TaskStatus::SOURCE_EXECUTOR);

    v1::executor::Call call;
    call.set_type(v1::executor::Call::UPDATE);
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
    call.mutable_update()->mutable_status()->CopyFrom(status);

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1/executor",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
    EXPECT_TRUE(strings::contains(response->body, failureMessage));
  }

  {
    v1::executor::Call call;
    call.set_type(v1::executor::Call::MESSAGE);
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
    call.mutable_message()->set_data("executor message");

    Future<http::Response> response = http::post(
      slave.get()->pid,
      "api/v1/executor",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::Forbidden().status, response);
    EXPECT_TRUE(strings::contains(response->body, failureMessage));
  }

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));
}
#endif // USE_SSL_SOCKET


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

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);
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

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
