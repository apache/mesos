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
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

#include "tests/master/mock_master_api_subscriber.hpp"

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using process::http::Accepted;
using process::http::Headers;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using testing::AtMost;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

class RoleTest : public MesosTest {};


// This test checks that a framework cannot register with a role that
// is not in the configured list.
TEST_F(RoleTest, BadRegister)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "invalid");

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
  frameworkInfo.set_roles(0, "new-role-name");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreserved = Resources::parse("disk:1024").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

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
      allocatedResources(unreserved, frameworkInfo.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));

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
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

  Resources volume = createPersistentVolume(
      Megabytes(64),
      frameworkInfo.roles(0),
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
      allocatedResources(volume, frameworkInfo.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(dynamicallyReserved, frameworkInfo.roles(0))));
  EXPECT_FALSE(Resources(offer.resources()).contains(
      allocatedResources(unreserved, frameworkInfo.roles(0))));

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
  frameworkInfo.set_roles(0, "role");

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
      allocatedResources(staticallyReserved, frameworkInfo.roles(0))));

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
// information when there are no known roles.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointEmpty)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "roles",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": []"
      "}");

  ASSERT_SOME(expected);

  EXPECT_EQ(*expected, *parse)
    << "expected " << stringify(*expected)
    << " vs actual " << stringify(*parse);
}


// This test checks that the "/roles" endpoint returns the expected
// information when the role whitelist is used but no frameworks
// are present.
TEST_F(RoleTest, EndpointWithWhitelistNoFrameworks)
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

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"*\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"*\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 1.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role1\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"role1\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 5.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role2\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"role2\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);

  EXPECT_EQ(*expected, *parse)
    << "expected " << stringify(*expected)
    << " vs actual " << stringify(*parse);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // Query the master roles endpoint and check it contains quota.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

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
              "\"role\":\"foo\","
              "\"consumed\": {},"
              "\"guarantee\":"
                "{"
                  "\"cpus\":1.0,"
                  "\"mem\":512.0"
                "},"
              "\"limit\":"
                "{"
                  "\"cpus\":1.0,"
                  "\"mem\":512.0"
                "}"
            "}"
        "}"
    );
    ASSERT_SOME(expected);

    EXPECT_TRUE(role.contains(*expected))
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(role);
  }
}


// This test ensures that quota consumption is included
// in /roles endpoint of master.
//
// We set up the following that should be included in
// the quota consumption:
//   - Allocated unreserved resources
//   - Allocated reservation
//   - Unallocated reservation
//
// And we set up the following that should not be included
// in the quota consumption:
//   - Outstanding offer
//
// TODO(bmahler): Test hierarchical accounting accuracy.
TEST_F(RoleTest, RolesEndpointContainsConsumedQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent with reserved resources and
  // allocate them to a task.
  slave::Flags agentFlags1 = CreateSlaveFlags();
  agentFlags1.resources = "cpus(role):1;mem(role):10;"
                          "disk:0;ports:[]";

  // We need to use the posix launcher to avoid agents
  // seeing each other's containers as orphans and
  // killing them.
  agentFlags1.launcher = "posix";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector.get(), agentFlags1);
  ASSERT_SOME(slave1);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  ExecutorInfo executorInfo = createExecutorInfo("dummy", "sleep 3600");

  Future<Nothing> task1Launched;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(LaunchTasks(executorInfo, 1, 1, 10, "role"),
                    FutureSatisfy(&task1Launched)));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(task1Launched);

  // Now we have:
  //  - Allocated reservation: cpus:1;mem:10

  // Start an agent with unreserved resources and allocate
  // them to a task.

  Future<Nothing> task2Launched;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(LaunchTasks(executorInfo, 1, 10, 100, "role"),
                    FutureSatisfy(&task2Launched)));

  slave::Flags agentFlags2 = CreateSlaveFlags();
  agentFlags2.resources = "cpus:10;mem:100;"
                          "disk:0;ports:[]";

  // We need to use the posix launcher to avoid agents
  // seeing each other's containers as orphans and
  // killing them.
  agentFlags2.launcher = "posix";

  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get(), agentFlags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(task2Launched);

  // Now we have:
  //  - Allocated reservation: cpus:1;mem:10
  //  - Allocated unreserved resources: cpus:10;mem:100

  // Start an agent with both reserved and unreserved
  // resources, but let them remain offered.

  Future<Nothing> offer;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&offer));

  slave::Flags agentFlags3 = CreateSlaveFlags();
  agentFlags3.resources = "cpus(role):100;mem(role):1000;ports(role):[100-199]"
                          ";cpus:1000;mem:10000;disk:0;ports:[1000-1999]";

  // We need to use the posix launcher to avoid agents
  // seeing each other's containers as orphans and
  // killing them.
  agentFlags3.launcher = "posix";

  Try<Owned<cluster::Slave>> slave3 = StartSlave(detector.get(), agentFlags3);
  ASSERT_SOME(slave3);

  AWAIT_READY(offer);

  // Now we have:
  //  - Allocated reservation: cpus:1;mem:10
  //  - Allocated unreserved resources: cpus:10;mem:100
  //  - Offered reservation: cpus:100;mem:1000;ports:100
  //  - Offered unreserved resources: cpus:1000;mem:10000;ports:1000

  // Check that the /roles endopint has the correct quota
  // consumption information.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": ["
        "    {"
        "      \"frameworks\": [\"" + frameworkId->value() + "\"],"
        "      \"name\": \"role\","
        "      \"resources\": {"
        "        \"cpus\": 1111.0,"
        "        \"mem\":  11110.0,"
        "        \"ports\": 1100.0"
        "      },"
        "      \"allocated\": {"
        "        \"cpus\": 11.0,"
        "        \"mem\":  110.0"
        "      },"
        "      \"offered\": {"
        "        \"cpus\": 1100.0,"
        "        \"mem\":  11000.0,"
        "        \"ports\": 1100.0"
        "      },"
        "      \"reserved\": {"
        "        \"cpus\": 101.0,"
        "        \"mem\":  1010.0,"
        "        \"ports\": 100.0"
        "      },"
        "      \"quota\": {"
        "        \"consumed\": {"
        "          \"cpus\": 111.0,"
        "          \"mem\": 1110.0,"
        "          \"ports\": 100.0"
        "        },"
        "        \"guarantee\": {},"
        "        \"limit\": {},"
        "        \"role\": \"role\""
        "      },"
        "      \"weight\": 1.0"
        "    }"
        "  ]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(*expected, *parse)
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(*parse);
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
  frameworkInfo1.set_roles(0, "roleX");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId1;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId1));

  driver1.start();

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_roles(0, "roleZ");

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

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [\"" + frameworkId1->value() + "\"],"
      "      \"name\": \"roleX\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"roleX\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 5.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"roleY\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"roleY\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 4.0"
      "    },"
      "    {"
      "      \"frameworks\": [\"" + frameworkId2->value() + "\"],"
      "      \"name\": \"roleZ\","
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"roleZ\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);

  EXPECT_EQ(*expected, *parse)
    << "expected " << stringify(*expected)
    << " vs actual " << stringify(*parse);

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
      "      \"resources\": {},"
      "      \"allocated\": {},"
      "      \"offered\": {},"
      "      \"reserved\": {},"
      "      \"quota\": {"
      "        \"role\": \"non-existent-role\","
      "        \"consumed\": {},"
      "        \"guarantee\": {},"
      "        \"limit\": {}"
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

  EXPECT_EQ(*expected, *parse)
    << "expected " << stringify(*expected)
    << " vs actual " << stringify(*parse);
}


// This test ensures that roles with only reservations are
// included in the /roles endpoint.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointImplicitRolesReservations)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::MockMasterAPISubscriber subscriber;

  AWAIT_READY(subscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(subscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus(role):1;mem(role):10";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);

  AWAIT_READY(agentAdded);

  // Check that the /roles endpoint contains the role.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": ["
        "    {"
        "      \"frameworks\": [],"
        "      \"name\": \"role\","
        "      \"resources\": {},"
        "      \"allocated\": {},"
        "      \"offered\": {},"
        "      \"reserved\": {"
        "        \"cpus\": 1.0,"
        "        \"mem\":  10.0"
        "      },"
        "      \"quota\": {"
        "        \"consumed\": {"
        "          \"cpus\": 1.0,"
        "          \"mem\": 10.0"
        "        },"
        "        \"guarantee\": {},"
        "        \"limit\": {},"
        "        \"role\": \"role\""
        "      },"
        "      \"weight\": 1.0"
        "    }"
        "  ]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(*expected, *parse)
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(*parse);
  }
}


// This test ensures that ancestor roles are exposed when
// there are no direct objects associated with them.
//
// TODO(bmahler): This currently only tests the reservation
// case, but we should also test the allocation, framework
// subsription, and quota/weight configuration cases.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, EndpointImplicitRolesAncestors)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::MockMasterAPISubscriber subscriber;

  AWAIT_READY(subscriber.subscribe(master.get()->pid));

  Future<Nothing> agentAdded;
  EXPECT_CALL(subscriber, agentAdded(_))
    .WillOnce(FutureSatisfy(&agentAdded));

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus(ancestor/child):1;mem(ancestor/child):10;";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);

  AWAIT_READY(agentAdded);

  // Check that the /roles endpoint contains the role and
  // its ancestor.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": ["
        "    {"
        "      \"frameworks\": [],"
        "      \"name\": \"ancestor\","
        "      \"resources\": {},"
        "      \"allocated\": {},"
        "      \"offered\": {},"
        "      \"reserved\": {"
        "        \"cpus\": 1.0,"
        "        \"mem\":  10.0"
        "      },"
        "      \"quota\": {"
        "        \"consumed\": {"
        "          \"cpus\": 1.0,"
        "          \"mem\": 10.0"
        "        },"
        "        \"guarantee\": {},"
        "        \"limit\": {},"
        "        \"role\": \"ancestor\""
        "      },"
        "      \"weight\": 1.0"
        "    },"
        "    {"
        "      \"frameworks\": [],"
        "      \"name\": \"ancestor/child\","
        "      \"resources\": {},"
        "      \"allocated\": {},"
        "      \"offered\": {},"
        "      \"reserved\": {"
        "        \"cpus\": 1.0,"
        "        \"mem\":  10.0"
        "      },"
        "      \"quota\": {"
        "        \"consumed\": {"
        "          \"cpus\": 1.0,"
        "          \"mem\": 10.0"
        "        },"
        "        \"guarantee\": {},"
        "        \"limit\": {},"
        "        \"role\": \"ancestor/child\""
        "      },"
        "      \"weight\": 1.0"
        "    }"
        "  ]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(*expected, *parse)
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(*parse);
  }
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
  framework.set_roles(0, "role1");
  framework.add_roles("role2");

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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": ["
        "    {"
        "      \"frameworks\": [\"" + frameworkId->value() + "\"],"
        "      \"name\": \"role1\","
        "      \"resources\": {},"
        "      \"allocated\": {},"
        "      \"offered\": {},"
        "      \"reserved\": {},"
        "      \"quota\": {"
        "        \"role\": \"role1\","
        "        \"consumed\": {},"
        "        \"guarantee\": {},"
        "        \"limit\": {}"
        "      },"
        "      \"weight\": 1.0"
        "    },"
        "    {"
        "      \"frameworks\": [\"" + frameworkId->value() + "\"],"
        "      \"name\": \"role2\","
        "      \"resources\": {},"
        "      \"allocated\": {},"
        "      \"offered\": {},"
        "      \"reserved\": {},"
        "      \"quota\": {"
        "        \"role\": \"role2\","
        "        \"consumed\": {},"
        "        \"guarantee\": {},"
        "        \"limit\": {}"
        "      },"
        "      \"weight\": 1.0"
        "    }"
        "  ]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(*expected, *parse)
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(*parse);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::parse(
        "{"
        "  \"roles\": []"
        "}");

    ASSERT_SOME(expected);

    EXPECT_EQ(*expected, *parse)
      << "expected " << stringify(*expected)
      << " vs actual " << stringify(*parse);  }
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


TEST_F(RoleTest, Ancestors)
{
  EXPECT_EQ(vector<string>(),
            roles::ancestors("role"));

  EXPECT_EQ(vector<string>({"a/b/c", "a/b", "a"}),
            roles::ancestors("a/b/c/d"));
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


TEST_F(RoleTest, isStrictSubroleOf)
{
  EXPECT_TRUE(roles::isStrictSubroleOf("foo/bar", "foo"));
  EXPECT_TRUE(roles::isStrictSubroleOf("foo/bar/baz", "foo"));
  EXPECT_FALSE(roles::isStrictSubroleOf("foo", "foo"));
  EXPECT_FALSE(roles::isStrictSubroleOf("bar", "foo"));
  EXPECT_FALSE(roles::isStrictSubroleOf("foobar", "foo"));
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


// This test confirms that our handling of persistent volumes from hierarchical
// roles does not cause leaking of volumes. Since hierarchical roles contain
// literal `/` an implementation not taking this into account could map the name
// of a hierarchical role `A/B` onto a directory hierarchy `A/B`. If the
// persistent volume with persistence id `ID` and role `ROLE` is mapped to a
// path `ROLE/ID`, it becomes impossible to distinguish the last component of a
// hierarchical role from a persistence id.
//
// This performs the following checks:
//
// 1) Run two tasks with volumes whose role and id overlap on the file system in
// the naive implementation. The tasks should not be able to see each others
// volumes.
//
// 2) Destroy the previously created volumes in an order such that the in the
// naive implementation less nested volume is destroyed first. This should not
// destroy the more nested volume (e.g., since it is not created as a
// subdirectory).
//
// TODO(bbannier): Figure out a way to run the test command on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(RoleTest, VolumesInOverlappingHierarchies)
{
  constexpr char PATH[] = "path";
  constexpr Bytes DISK_SIZE = Megabytes(1);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Capture the SlaveID so we can use it in create/destroy volumes API calls.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Helper function which starts a framework in a role and creates a
  // persistent volume with the given id. The framework creates a task
  // using the volume and makes sure that no volumes from other roles
  // are leaked into the volume.
  auto runTask = [&master, &PATH](
      const string& role, const string& id) {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_roles(0, role);

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

    // Create a reserved disk. We only create a small disk so
    // we have remaining disk to offer to other frameworks.
    Resource reservedDisk = createReservedResource(
        "disk",
        "1",
        createDynamicReservationInfo(role, DEFAULT_CREDENTIAL.principal()));

    // Create a persistent volume on the reserved disk.
    Resource volume = createPersistentVolume(
        reservedDisk,
        id,
        PATH,
        DEFAULT_CREDENTIAL.principal(),
        frameworkInfo.principal());

    // Create a task which uses the volume and checks that
    // it contains no files leaked from another role.
    const Offer& offer = offers.get()[0];

    Resources cpusMem =
      Resources(offer.resources()).filter([](const Resource& r) {
        return r.name() == "cpus" || r.name() == "mem";
      });

    Resources taskResources = cpusMem + volume;

    // Create a task confirming that the directory `path` is empty.
    // Note that we do not explicitly confirm that `path` exists here.
    TaskInfo task = createTask(
        offer.slave_id(),
        taskResources,
        "! (ls -Av path | grep -q .)");

    // We expect three status updates for the task.
    Future<TaskStatus> status0, status1, status2;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&status0))
      .WillOnce(FutureArg<1>(&status1))
      .WillOnce(FutureArg<1>(&status2));

    // Accept the offer.
    driver.acceptOffers(
        {offer.id()},
        {RESERVE(reservedDisk), CREATE(volume), LAUNCH({task})});

    AWAIT_READY(status0);

    EXPECT_EQ(task.task_id(), status0->task_id());
    EXPECT_EQ(TASK_STARTING, status0->state());

    AWAIT_READY(status1);

    EXPECT_EQ(task.task_id(), status1->task_id());
    EXPECT_EQ(TASK_RUNNING, status1->state());

    AWAIT_READY(status2);

    EXPECT_EQ(task.task_id(), status2->task_id());
    EXPECT_EQ(TASK_FINISHED, status2->state())
      << "Task for role '" << role << "' and id '" << id << "' did not succeed";

    driver.stop();
    driver.join();
  };

  // Helper function to destroy a volume with given role and id.
  auto destroyVolume = [&slaveId, &master, &PATH, DISK_SIZE](
      const string& role, const string& id) {
    Resource volume = createPersistentVolume(
        DISK_SIZE,
        role,
        id,
        PATH,
        DEFAULT_CREDENTIAL.principal(),
        None(),
        DEFAULT_CREDENTIAL.principal());

    v1::master::Call destroyVolumesCall;
    destroyVolumesCall.set_type(v1::master::Call::DESTROY_VOLUMES);

    v1::master::Call::DestroyVolumes* destroyVolumes =
      destroyVolumesCall.mutable_destroy_volumes();
    destroyVolumes->add_volumes()->CopyFrom(evolve(volume));
    destroyVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));

    constexpr ContentType CONTENT_TYPE = ContentType::PROTOBUF;

    Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(CONTENT_TYPE);

    return process::http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(CONTENT_TYPE, destroyVolumesCall),
        stringify(CONTENT_TYPE));
  };

  // Run two tasks. In the naive storage scheme of volumes from hierarchical
  // role frameworks, the first volume would be created under paths
  // `a/b/c/d/id/` and the second one under `a/b/c/d/`. The second task would in
  // that case incorrectly see a directory `id` in its persistent volume.
  runTask("a/b/c/d", "id");
  runTask("a/b/c", "d");

  // Destroy both volumes. Even though the role `a/b/c` is a prefix of the role
  // `a/b/c/d`, destroying the former role's volume `d` should not interfere
  // with the latter's volume `id`.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Accepted().status, destroyVolume("a/b/c", "d"));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Accepted().status, destroyVolume("a/b/c/d", "id"));
}

}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
