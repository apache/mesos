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

#include <mesos/http.hpp>
#include <mesos/roles.hpp>

#include <process/clock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using testing::AtMost;

namespace mesos {
namespace internal {
namespace tests {

class RoleTest : public MesosTest {};


// This test checks that a framework cannot register with a role that
// is not in the configured list.
TEST_F(RoleTest, BadRegister)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("invalid");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "foo,bar";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();
}


// This test checks that when using implicit roles, a framework can
// register with a new role, make a dynamic reservation, and create a
// persistent volume.
TEST_F(RoleTest, ImplicitRoleRegister)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:512;disk:1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("new-role-name");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved = unreserved.flatten(
      frameworkInfo.role(),
      createReservationInfo(frameworkInfo.principal())).get();

  // We use this to capture offers from `resourceOffers`.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {RESERVE(dynamicallyReserved)}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.role(),
      "id1",
      "path1",
      frameworkInfo.principal(),
      None(),
      frameworkInfo.principal());

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {CREATE(volume)}, filters);

  // In the next offer, expect an offer with a persistent volume.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(volume, frameworkInfo.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.role())));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.role())));

  driver.stop();
  driver.join();
}


// This test checks that when using implicit roles, a static
// reservation for a new role can be made and used to launch a task.
TEST_F(RoleTest, ImplicitRoleStaticReservation)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus(role):1;mem(role):512";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources staticallyReserved =
    Resources::parse(slaveFlags.resources.get()).get();

  // We use this to capture offers from `resourceOffers`.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers.get()[0];

  EXPECT_TRUE(Resources(offer.resources()).contains(
      allocatedResources(staticallyReserved, frameworkInfo.role())));

  // Create a task to launch with the resources of `staticallyReserved`.
  TaskInfo taskInfo =
    createTask(offer.slave_id(), staticallyReserved, "exit 1", exec.id);

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<TaskInfo> launchTask;

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&launchTask));

  driver.acceptOffers({offer.id()}, {LAUNCH({taskInfo})}, filters);

  AWAIT_READY(launchTask);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test checks that the "/roles" endpoint returns the expected
// information when there are no active roles.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointEmpty)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": []"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());
}


// This test checks that the "/roles" endpoint returns the expected
// information when there are configured weights and explicit roles,
// but no registered frameworks.
TEST_F(RoleTest, EndpointNoFrameworks)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "role1,role2";
  masterFlags.weights = "role1=5";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"*\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role1\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 5.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role2\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());
}


// This test ensures that quota information is included
// in /roles endpoint of master.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, RolesEndpointContainsQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Resources quotaResources = Resources::parse("cpus:1;mem:512").get();

  mesos::quota::QuotaRequest request;
  request.set_role("foo");
  request.mutable_guarantee()->CopyFrom(quotaResources);

  // Use the force flag for setting quota that cannot be satisfied in
  // this empty cluster without any agents.
  request.set_force(true);

  {
    Future<Response> response = process::http::post(
        master.get()->pid,
        "quota",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        stringify(JSON::protobuf(request)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;
  }

  // Query the master roles endpoint and check it contains quota.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Array> roles = parse->find<JSON::Array>("roles");
    ASSERT_SOME(roles);
    EXPECT_EQ(1u, roles->values.size());

    JSON::Value role = roles->values[0].as<JSON::Value>();

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"quota\":"
            "{"
              "\"guarantee\":"
                "{"
                  "\"cpus\":1.0,"
                  "\"disk\":0,"
                  "\"gpus\":0,"
                  "\"mem\":512.0"
                "},"
              "\"principal\":\"test-principal\","
              "\"role\":\"foo\""
            "}"
        "}"
    );
    ASSERT_SOME(expected);

    EXPECT_TRUE(role.contains(expected.get()));
  }
}


// This test checks that when using implicit roles, the "/roles"
// endpoint shows roles that have a configured weight even if they
// have no registered frameworks.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointImplicitRolesWeights)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.weights = "roleX=5,roleY=4";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_role("roleX");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId1;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId1));

  driver1.start();

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_role("roleZ");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId2;
  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .WillOnce(FutureArg<1>(&frameworkId2));

  driver2.start();

  AWAIT_READY(frameworkId1);
  AWAIT_READY(frameworkId2);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [\"" + frameworkId1->value() + "\"],"
      "      \"name\": \"roleX\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 5.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"roleY\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 4.0"
      "    },"
      "    {"
      "      \"frameworks\": [\"" + frameworkId2->value() + "\"],"
      "      \"name\": \"roleZ\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This test checks that when using implicit roles, the "/roles"
// endpoint shows roles that have a configured quota even if they have
// no registered frameworks.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointImplicitRolesQuotas)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Resources quotaResources = Resources::parse("cpus:1;mem:512").get();
  const RepeatedPtrField<Resource>& jsonQuotaResources =
    static_cast<const RepeatedPtrField<Resource>&>(quotaResources);

  // Send a quota request for a new role name. Note that we set
  // "force" to true because we're setting a quota that can't
  // currently be satisfied by the resources in the cluster (because
  // there are no slaves registered).
  string quotaRequestBody = strings::format(
      "{\"role\":\"non-existent-role\",\"guarantee\":%s,\"force\":true}",
      JSON::protobuf(jsonQuotaResources)).get();

  Future<Response> quotaResponse = process::http::post(
      master.get()->pid,
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      quotaRequestBody);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, quotaResponse)
    << quotaResponse->body;

  Future<Response> rolesResponse = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, rolesResponse)
    << rolesResponse->body;

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON, "Content-Type", rolesResponse);

  Try<JSON::Value> parse = JSON::parse(rolesResponse->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"non-existent-role\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"gpus\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_TRUE(parse->contains(expected.get()));

  // Remove the quota, and check that the role no longer appears in
  // the "/roles" endpoint.
  Future<Response> deleteResponse = process::http::requestDelete(
      master.get()->pid,
      "quota/non-existent-role",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, deleteResponse)
    << deleteResponse->body;

  rolesResponse = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, rolesResponse)
    << rolesResponse->body;

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON, "Content-Type", rolesResponse);

  parse = JSON::parse(rolesResponse->body);
  ASSERT_SOME(parse);

  expected = JSON::parse(
      "{"
      "  \"roles\": []"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());
}


// This test ensures that master adds/removes all roles of
// a multi-role framework when it registers/terminates.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    RoleTest,
    AddAndRemoveFrameworkWithMultipleRoles)
{
  // When we test removeFramework later in this test, we have to
  // be sure the teardown call is processed completely before
  // sending request to `getRoles` API.
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_roles("role1");
  framework.add_roles("role2");
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));;

  driver.start();

  AWAIT_READY(frameworkId);

  // Tests all roles of the multi-role framework are added.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": ["
        "    {"
        "      \"frameworks\": [\"" + frameworkId->value() + "\"],"
        "      \"name\": \"role1\","
        "      \"resources\": {"
        "        \"cpus\": 0,"
        "        \"disk\": 0,"
        "        \"gpus\": 0,"
        "        \"mem\":  0"
        "      },"
        "      \"weight\": 1.0"
        "    },"
        "    {"
        "      \"frameworks\": [\"" + frameworkId->value() + "\"],"
        "      \"name\": \"role2\","
        "      \"resources\": {"
        "        \"cpus\": 0,"
        "        \"disk\": 0,"
        "        \"gpus\": 0,"
        "        \"mem\":  0"
        "      },"
        "      \"weight\": 1.0"
        "    }"
        "  ]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(expected.get(), parse.get());
  }

  // Set expectation that Master receives teardown call.
  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  driver.stop();
  driver.join();

  // Wait for teardown call to be dispatched.
  AWAIT_READY(teardownCall);

  // Make sure the teardown call is processed completely.
  Clock::settle();

  // Tests all roles of multi-role framework are removed.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response->body;

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": []"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(expected.get(), parse.get());
  }
}


// This tests the parse function of roles.
TEST_F(RoleTest, Parsing)
{
  vector<string> v = {"abc", "edf", "hgi"};

  Try<vector<string>> r1 = roles::parse("abc,edf,hgi");
  EXPECT_SOME_EQ(v, r1);

  Try<vector<string>> r2 = roles::parse("abc,edf,hgi,");
  EXPECT_SOME_EQ(v, r2);

  Try<vector<string>> r3 = roles::parse(",abc,edf,hgi");
  EXPECT_SOME_EQ(v, r3);

  Try<vector<string>> r4 = roles::parse("abc,,edf,hgi");
  EXPECT_SOME_EQ(v, r4);

  EXPECT_ERROR(roles::parse("foo,.,*"));
}


// This tests the validate functions of roles. Keep in mind that
// roles::validate returns Option<Error>, so it will return None() when
// validation succeeds, or Error() when validation fails.
TEST_F(RoleTest, Validate)
{
  EXPECT_NONE(roles::validate("foo"));
  EXPECT_NONE(roles::validate("123"));
  EXPECT_NONE(roles::validate("foo_123"));
  EXPECT_NONE(roles::validate("*"));
  EXPECT_NONE(roles::validate("foo-bar"));
  EXPECT_NONE(roles::validate("foo.bar"));
  EXPECT_NONE(roles::validate("foo..bar"));
  EXPECT_NONE(roles::validate("..."));
  EXPECT_NONE(roles::validate("foo/bar"));
  EXPECT_NONE(roles::validate("foo/.bar"));
  EXPECT_NONE(roles::validate("foo/bar/baz"));
  EXPECT_NONE(roles::validate("a/b/c/d/e/f/g/h"));

  EXPECT_SOME(roles::validate(""));
  EXPECT_SOME(roles::validate("."));
  EXPECT_SOME(roles::validate(".."));
  EXPECT_SOME(roles::validate("-foo"));
  EXPECT_SOME(roles::validate("/"));
  EXPECT_SOME(roles::validate("/foo"));
  EXPECT_SOME(roles::validate("foo/"));
  EXPECT_SOME(roles::validate("foo//bar"));
  EXPECT_SOME(roles::validate("foo/bar/"));
  EXPECT_SOME(roles::validate("foo bar"));
  EXPECT_SOME(roles::validate("foo\tbar"));
  EXPECT_SOME(roles::validate("foo\nbar"));

  EXPECT_SOME(roles::validate("./."));
  EXPECT_SOME(roles::validate("../.."));
  EXPECT_SOME(roles::validate("./foo"));
  EXPECT_SOME(roles::validate("../foo"));
  EXPECT_SOME(roles::validate("foo/."));
  EXPECT_SOME(roles::validate("foo/.."));
  EXPECT_SOME(roles::validate("foo/./bar"));
  EXPECT_SOME(roles::validate("foo/../bar"));
  EXPECT_SOME(roles::validate("foo/-bar"));
  EXPECT_SOME(roles::validate("foo/*"));
  EXPECT_SOME(roles::validate("foo/*/bar"));
  EXPECT_SOME(roles::validate("*/foo"));

  EXPECT_NONE(roles::validate({"foo", "bar", "*"}));

  EXPECT_SOME(roles::validate({"foo", ".", "*"}));
}


// Testing get without authentication and with bad credentials.
TEST_F(RoleTest, EndpointBadAuthentication)
{
  // Set up a master with authentication required.
  // Note that the default master test flags enable HTTP authentication.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Get request without authentication.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "roles");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  // Bad credentials which should fail authentication.
  Credential badCredential;
  badCredential.set_principal("badPrincipal");
  badCredential.set_secret("badSecret");

  // Get request with bad authentication.
  response = process::http::get(
    master.get()->pid,
    "roles",
    None(),
    createBasicAuthHeaders(badCredential));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}

}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
