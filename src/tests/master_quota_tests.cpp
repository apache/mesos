/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/quota/quota.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>

#include <stout/format.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"

using std::string;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::quota::QuotaInfo;

using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::Conflict;
using process::http::OK;
using process::http::Response;

using testing::_;
using testing::DoAll;
using testing::Eq;

namespace mesos {
namespace internal {
namespace tests {

// Quota tests that are allocator-agnostic (i.e. we expect every
// allocator to implement basic quota guarantees) are in this
// file. All tests are split into logical groups:
//   * Request validation tests.
//   * Sanity check tests.
//   * Quota functionality tests.
//   * Failover, and recovery tests.

// TODO(alexr): Once we have other allocators, convert this test into a
// typed test over multiple allocators.
class MasterQuotaTest : public MesosTest
{
protected:
  MasterQuotaTest()
  {
    // We reuse default agent resources and expect them to be sufficient.
    defaultAgentResources = Resources::parse(defaultAgentResourcesString).get();
    CHECK(defaultAgentResources.contains(Resources::parse(
        "cpus:2;mem:1024;disk:1024;ports:[31000-32000]").get()));
  }

  // Sets up the master flags with two roles and a short allocation interval.
  virtual master::Flags CreateMasterFlags()
  {
    master::Flags flags = MesosTest::CreateMasterFlags();
    flags.allocation_interval = Milliseconds(50);
    flags.roles = strings::join(",", ROLE1, ROLE2);
    return flags;
  }

  process::http::Headers createBasicAuthHeaders(
      const Credential& credential) const
  {
    return process::http::Headers{{
      "Authorization",
      "Basic " +
        base64::encode(credential.principal() + ":" + credential.secret())
    }};
  }

  // Creates a FrameworkInfo with the specified role.
  FrameworkInfo createFrameworkInfo(const string& role)
  {
    FrameworkInfo info;
    info.set_user("user");
    info.set_name("framework" + process::ID::generate());
    info.mutable_id()->set_value(info.name());
    info.set_role(role);

    return info;
  }

  // Generates a quota request from the specified resources.
  string createRequestBody(const Resources& resources) const
  {
    return strings::format("resources=%s", JSON::protobuf(
        static_cast<const RepeatedPtrField<Resource>&>(resources))).get();
  }

protected:
  const std::string ROLE1{"role1"};
  const std::string ROLE2{"role2"};

  Resources defaultAgentResources;
};


// These are request validation tests. They verify JSON is well-formed,
// convertible to corresponding protobufs, all necessary fields are present,
// while irrelevant fields are not present.

// TODO(alexr): Tests to implement:
//   * Role is absent.
//   * Role is an empty string.
//   * Role is '*'?
//   * Resources with the same name are present.

// Verifies that a request for a non-existent role is rejected.
// TODO(alexr): This may be revisited once we allow dynamic roles and
// therefore allow setting quota before a role is known to the master.
TEST_F(MasterQuotaTest, NonExistentRole)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We do not need an agent since a request should be rejected before we
  // start looking at available resources.

  // We request quota for a portion of resources available on the agent.
  Resources quotaResources =
    Resources::parse("cpus:1;mem:512", "non-existent-role").get();

  // Send a quota request for the specified role.
  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  Shutdown();
}


// Quota requests with invalid structure should return a '400 Bad Request'.
TEST_F(MasterQuotaTest, SetInvalidRequest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We do not need an agent since a request should be rejected before
  // we start looking at available resources.

  // We wrap the `http::post` into a lambda for readability of the tests.
  auto postQuota = [this, &master](const string& request) {
    return process::http::post(
        master.get(),
        "quota",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        request);
  };

  // Tests whether a quota request with missing 'resource=[]' fails.
  {
    string badRequest =
      "{"
      "  invalidJson"
      "}";

    Future<Response> response = postQuota(badRequest);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  // Tests whether a quota requests with invalid json fails.
  {
    string badRequest =
      "resources=["
      "  \"invalidJson\" : 1"
      "]";

    Future<Response> response = postQuota(badRequest);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  // Tests whether a quota request with invalid resources fails.
  {
    string badRequest =
      "resources=["
      "  {\"invalidResource\" : 1}"
      "]";

    Future<Response> response = postQuota(badRequest);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  Shutdown();
}


// A quota request with non-scalar resources should return a '400 Bad Request'.
TEST_F(MasterQuotaTest, SetNonScalar)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We do not need an agent since a request should be rejected before
  // we start looking at available resources.

  // Quota set request including non-scalar port resources.
  Resources quotaResources =
    Resources::parse("cpus:1;mem:512;ports:[31000-31001]", ROLE1).get();

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  Shutdown();
}


// A quota request with multiple roles should return a '400 Bad Request'.
TEST_F(MasterQuotaTest, SetMultipleRoles)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We do not need an agent since a request should be rejected before
  // we start looking at available resources.

  // Create a quota request with resources belonging to different roles.
  Resources quotaResources = Resources::parse("cpus:1;mem:512;", ROLE1).get();
  quotaResources += Resources::parse("cpus:1;mem:512;", ROLE2).get();

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  Shutdown();
}


// Updating an exiting quota via POST to the '/master/quota endpoint' should
// return a '400 BadRequest'.
TEST_F(MasterQuotaTest, SetExistingQuota)
{
  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  // Wait until the agent registers.
  Future<Resources> agentTotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agentTotalResources)));

  Try<PID<Slave>> agent = StartSlave();
  ASSERT_SOME(agent);

  AWAIT_READY(agentTotalResources);
  EXPECT_EQ(defaultAgentResources, agentTotalResources.get());

  // We request quota for a portion of resources available on the agent.
  Resources quotaResources = Resources::parse("cpus:1;mem:512;", ROLE1).get();
  EXPECT_TRUE(agentTotalResources.get().contains(quotaResources.flatten()));

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response) << response.get().body;

  // Try to set quota via post a second time.
  response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  Shutdown();
}


// Checks whether a quota request with any invalid field set is rejected:
//   * `ReservationInfo`.
//   * `RevocableInfo`.
//   * `DiskInfo`.
TEST_F(MasterQuotaTest, SetInvalidResourceInfos)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We do not need an agent since a request should be rejected before
  // we start looking at available resources.

  // Create a quota set request with `DiskInfo` and check that the
  // request returns a '400 Bad Request' return code.
  {
    Resources quotaResources = Resources::parse("cpus:1;mem:512", ROLE1).get();

    Resource volume = Resources::parse("disk", "128", ROLE1).get();
    volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
    quotaResources += volume;

    Future<Response> response = process::http::post(
        master.get(),
        "quota",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        createRequestBody(quotaResources));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  // Create a quota set request with `RevocableInfo` and check that
  // the request returns a '400 Bad Request' return code.
  {
    Resources quotaResources = Resources::parse("cpus:1;mem:512", ROLE1).get();

    Resource revocable = Resources::parse("cpus", "1", ROLE1).get();
    revocable.mutable_revocable();
    quotaResources += revocable;

    Future<Response> response = process::http::post(
        master.get(),
        "quota",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        createRequestBody(quotaResources));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  // Create a quota set request with `ReservationInfo` and check that
  // the request returns a '400 Bad Request' return code.
  {
    Resources quotaResources = Resources::parse("cpus:4;mem:512", ROLE1).get();

    Resource volume = Resources::parse("disk", "128", ROLE1).get();
    volume.mutable_reservation()->CopyFrom(
        createReservationInfo(DEFAULT_CREDENTIAL.principal()));

    quotaResources += volume;

    Future<Response> response = process::http::post(
        master.get(),
        "quota",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        createRequestBody(quotaResources));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
      << response.get().body;
  }

  Shutdown();
}


// These tests check whether a request makes sense in terms of current cluster
// status. A quota request may be well-formed, but obviously infeasible, e.g.
// request for 100 CPUs in a cluster with just 11 CPUs.

// TODO(alexr): Tests to implement:
//   * Sufficient total resources, but insufficient free resources due to
//     running tasks (multiple agents).
//   * Sufficient total resources, but insufficient free resources due to
//     dynamic reservations.
//   * Sufficient with static but insufficient without (static reservations
//     are not included).
//   * Multiple quotas in the cluster, sufficient free resources for a new
//     request.
//   * Multiple quotas in the cluster, insufficient free resources for a new
//     request.
//   * Multiple quotas in the cluster, sufficient free resources for a new
//     request, but some resources are blocked in outstanding offers
//     (rescinding).
//   * Sanity check is disabled with the `--force` flag.
//   * Deactivated or disconnected agents are not considered during quota
//     capability heuristics.

// Checks that a quota request is not satisfied if there are not enough
// resources.
TEST_F(MasterQuotaTest, InsufficientResourcesSingleAgent)
{
  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  // Start an agent and wait until it registers.
  Future<Resources> agentTotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agentTotalResources)));

  Try<PID<Slave>> agent = StartSlave();
  ASSERT_SOME(agent);

  AWAIT_READY(agentTotalResources);
  EXPECT_EQ(defaultAgentResources, agentTotalResources.get());

  // Our quota request requires more resources than available on the agent
  // (and in the cluster).
  Resources quotaResources =
    agentTotalResources.get().filter(
        [=](const Resource& resource) {
          return (resource.name() == "cpus" || resource.name() == "mem");
        }) +
    Resources::parse("cpus:1;mem:1024").get();

  quotaResources = quotaResources.flatten(ROLE1);

  EXPECT_FALSE(agentTotalResources.get().contains(quotaResources.flatten()));

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response)
    << response.get().body;

  Shutdown();
}


// Checks that a quota request is not satisfied if there are not enough
// resources.
TEST_F(MasterQuotaTest, InsufficientResourcesMultipleAgents)
{
  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  // Start one agent and wait until it registers.
  Future<Resources> agent1TotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agent1TotalResources)));

  Try<PID<Slave>> agent1 = StartSlave();
  ASSERT_SOME(agent1);

  AWAIT_READY(agent1TotalResources);
  EXPECT_EQ(defaultAgentResources, agent1TotalResources.get());

  // Start another agent and wait until it registers.
  Future<Resources> agent2TotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agent2TotalResources)));

  Try<PID<Slave>> agent2 = StartSlave();
  ASSERT_SOME(agent2);

  AWAIT_READY(agent2TotalResources);
  EXPECT_EQ(defaultAgentResources, agent2TotalResources.get());

  // Our quota request requires more resources than available on the agent
  // (and in the cluster).
  Resources quotaResources =
    agent1TotalResources.get().filter([=](const Resource& resource) {
      return (resource.name() == "cpus" || resource.name() == "mem");
    }) +
    agent2TotalResources.get().filter([=](const Resource& resource) {
      return (resource.name() == "cpus" || resource.name() == "mem");
    }) +
    Resources::parse("cpus:1;mem:1024").get();

  quotaResources = quotaResources.flatten(ROLE1);
  EXPECT_FALSE((agent1TotalResources.get() + agent2TotalResources.get())
    .contains(quotaResources.flatten()));

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Conflict().status, response)
    << response.get().body;

  Shutdown();
}


// Checks that an operator can request quota when enough resources are
// available on single agent.
TEST_F(MasterQuotaTest, AvailableResourcesSingleAgent)
{
  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  // Start an agent and wait until it registers.
  Future<Resources> agentTotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agentTotalResources)));

  Try<PID<Slave>> agent = StartSlave();
  ASSERT_SOME(agent);

  AWAIT_READY(agentTotalResources);
  EXPECT_EQ(defaultAgentResources, agentTotalResources.get());

  // We request quota for a portion of resources available on the agent.
  Resources quotaResources = Resources::parse("cpus:1;mem:512", ROLE1).get();
  EXPECT_TRUE(agentTotalResources.get().contains(quotaResources.flatten()));

  // Send a quota request for the specified role.
  Future<QuotaInfo> receivedQuotaRequest;
  EXPECT_CALL(allocator, setQuota(Eq(ROLE1), _))
    .WillOnce(DoAll(InvokeSetQuota(&allocator),
                    FutureArg<1>(&receivedQuotaRequest)));

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response) << response.get().body;

  // Quota request is granted and reached the allocator. Make sure nothing
  // got lost in-between.
  AWAIT_READY(receivedQuotaRequest);

  EXPECT_EQ(ROLE1, receivedQuotaRequest.get().role());
  EXPECT_EQ(quotaResources, Resources(receivedQuotaRequest.get().guarantee()));

  Shutdown();
}


// Checks that an operator can request quota when enough resources are
// available in the cluster, but not on a single agent.
TEST_F(MasterQuotaTest, AvailableResourcesMultipleAgents)
{
  TestAllocator<> allocator;
  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  // Start one agent and wait until it registers.
  Future<Resources> agent1TotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agent1TotalResources)));

  Try<PID<Slave>> agent1 = StartSlave();
  ASSERT_SOME(agent1);

  AWAIT_READY(agent1TotalResources);
  EXPECT_EQ(defaultAgentResources, agent1TotalResources.get());

  // Start another agent and wait until it registers.
  Future<Resources> agent2TotalResources;
  EXPECT_CALL(allocator, addSlave(_, _, _, _, _))
    .WillOnce(DoAll(InvokeAddSlave(&allocator),
                    FutureArg<3>(&agent2TotalResources)));

  Try<PID<Slave>> agent2 = StartSlave();
  ASSERT_SOME(agent2);

  AWAIT_READY(agent2TotalResources);
  EXPECT_EQ(defaultAgentResources, agent2TotalResources.get());

  // We request quota for a portion of resources, which is not available
  // on a single agent.
  Resources quotaResources =
    agent1TotalResources.get().filter([=](const Resource& resource) {
      return (resource.name() == "cpus" || resource.name() == "mem");
    }) +
    agent2TotalResources.get().filter([=](const Resource& resource) {
      return (resource.name() == "cpus" || resource.name() == "mem");
    });

  quotaResources = quotaResources.flatten(ROLE1);

  // Send a quota request for the specified role.
  Future<QuotaInfo> receivedQuotaRequest;
  EXPECT_CALL(allocator, setQuota(Eq(ROLE1), _))
    .WillOnce(DoAll(InvokeSetQuota(&allocator),
                    FutureArg<1>(&receivedQuotaRequest)));

  Future<Response> response = process::http::post(
      master.get(),
      "quota",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      createRequestBody(quotaResources));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response) << response.get().body;

  // Quota request is granted and reached the allocator. Make sure nothing
  // got lost in-between.
  AWAIT_READY(receivedQuotaRequest);

  EXPECT_EQ(ROLE1, receivedQuotaRequest.get().role());
  EXPECT_EQ(quotaResources, Resources(receivedQuotaRequest.get().guarantee()));

  Shutdown();
}


// These tests ensure quota implements declared functionality. Note that the
// tests here are allocator-agnostic, which means we expect every allocator to
// implement basic quota guarantees.

// TODO(alexr): Tests to implement:
//   * An agent with quota'ed tasks disconnects and there are not enough free
//     resources (alert and under quota situation).
//   * An agent with quota'ed tasks disconnects and there are enough free
//     resources (new offers).
//   * Role quota is below its allocation (InverseOffer generation).
//   * Two roles, two frameworks, one is production but rejects offers, the
//     other is greedy and tries to hijack the cluster which is prevented by
//     quota.
//   * Quota'ed and non-quota'ed roles, multiple frameworks in quota'ed role,
//     ensure total allocation sums up to quota.
//   * Remove quota with no running tasks.
//   * Remove quota with running tasks.


// These tests verify the behavior in presence of master failover and recovery.

// TODO(alexr): Tests to implement:
//   * During the recovery, no overcommitment of resources should happen.
//   * During the recovery, no allocation of resources potentially needed to
//     satisfy quota should happen.
//   * If a cluster is under quota before the failover, it should be under quota
//     during the recovery (total quota sanity check).
//   * Master fails simultaneously with multiple agents, rendering the cluster
//     under quota (total quota sanity check).

} // namespace tests {
} // namespace internal {
} // namespace mesos {
