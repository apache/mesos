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

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>
#include <process/owned.hpp>

#include <stout/format.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;
using process::Owned;
using process::UPID;

using process::http::BadRequest;
using process::http::CaseInsensitiveEqual;
using process::http::CaseInsensitiveHash;
using process::http::Conflict;
using process::http::Forbidden;
using process::http::Headers;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Eq;

namespace mesos {
namespace internal {
namespace tests {

// The dynamic weights tests in this file are split into logical groups:
//   * Request validation tests.
//   * Sanity check tests.
//   * Dynamic weights functionality tests.
//   * Failover and recovery tests.
//   * Authentication and authorization tests.
class DynamicWeightsTest : public MesosTest
{
protected:
  DynamicWeightsTest() {}

  void checkWithGetRequest(
      const PID<Master>& master,
      const Credential& credential,
      const Option<string>& _weights = None())
  {
    Future<Response> response = process::http::request(
        process::http::createRequest(
            master,
            "GET",
            false,
            "weights",
            createBasicAuthHeaders(credential)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> parse = JSON::parse(response->body);
    ASSERT_SOME(parse);

    // Create Protobuf representation of weights.
    Try<RepeatedPtrField<WeightInfo>> weightInfos =
      ::protobuf::parse<RepeatedPtrField<WeightInfo>>(parse.get());

    ASSERT_SOME(weightInfos);

    hashmap<string, double> weights =
      convertToHashmap(weightInfos.get());

    if (_weights.isNone()) {
      EXPECT_TRUE(weights.empty());
    } else if (_weights == GET_WEIGHTS1) {
      EXPECT_EQ(1u, weights.size());
      EXPECT_EQ(2.0, weights["role1"]);
    } else if (_weights == GET_WEIGHTS2) {
      EXPECT_EQ(1u, weights.size());
      EXPECT_EQ(4.0, weights["role2"]);
    } else if (_weights == UPDATED_WEIGHTS1) {
      EXPECT_EQ(2u, weights.size());
      EXPECT_EQ(2.0, weights["role1"]);
      EXPECT_EQ(4.0, weights["role2"]);
    } else if (_weights == UPDATED_WEIGHTS2) {
      EXPECT_EQ(3u, weights.size());
      EXPECT_EQ(1.0, weights["role1"]);
      EXPECT_EQ(4.0, weights["role2"]);
      EXPECT_EQ(2.5, weights["role3"]);
    } else {
      EXPECT_EQ(_weights.get(), "Unexpected weights string.");
    }
  }

protected:
  const string ROLE1 = "role1";
  const string ROLE2 = "role2";
  const string GET_WEIGHTS1 = "role1=2.0";
  const string GET_WEIGHTS2 = "role2=4.0";
  const string UPDATED_WEIGHTS1 = "role1=2.0,role2=4.0";
  const string UPDATED_WEIGHTS2 = "role1=1.0,role3=2.5";
};


// Update weights requests with invalid JSON structure
// should return '400 Bad Request'.
TEST_F(DynamicWeightsTest, PutInvalidRequest)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Tests whether an update weights request with invalid JSON fails.
  string badRequest =
    "[{"
    "  \"weight\":3.2,"
    "  \"role\""
    "}]";

  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          badRequest));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);

  // Tests whether an update weights request with an invalid field fails.
  // In this case, the correct field name should be 'role'.
  badRequest =
    "[{"
    "  \"weight\":3.2,"
    "  \"invalidField\":\"role1\""
    "}]";

  response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          badRequest));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// An update weights request with zero value
// should return '400 Bad Request'.
TEST_F(DynamicWeightsTest, ZeroWeight)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a request to update the weight of 'role1' to 0.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos("role1=0");
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// An update weights request with negative value
// should return '400 Bad Request'.
TEST_F(DynamicWeightsTest, NegativeWeight)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a request to update the weight of 'role1' to -2.0.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos("role1=-2.0");
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// An update weights request with non-numeric value
// should return '400 Bad Request'.
TEST_F(DynamicWeightsTest, NonNumericWeight)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a request to update the weight of 'role1' to 'two'.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos("role1=two");
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// Updates must be explicit about what role they are updating,
// and a missing role request should return '400 Bad Request'.
TEST_F(DynamicWeightsTest, MissingRole)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a missing role update request.
  Future<Response> response1 = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          "weights=[{\"weight\":2.0}]"));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response1);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);

  // Send an empty role (only a space) update request.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(" =2.0");
  Future<Response> response2 = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response2);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// Verifies that an update request for an unknown role (not specified
// in --roles flag) is rejected when using explicit roles (--roles flag
// is specified when master is started).
TEST_F(DynamicWeightsTest, UnknownRole)
{
  // Specify --roles whitelist when starting master.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.roles = strings::join(",", ROLE1, ROLE2);
  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Send a weight update request for 'unknown' role.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos("unknown=3.0");
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// Verifies that a weights update request for a whitelisted role
// (contained in --roles flag) succeeds when using explicit roles
// (--roles flag is specified when master is started).
TEST_F(DynamicWeightsTest, UpdateWeightsWithExplictRoles)
{
  // Specify --roles whitelist when starting master.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.roles = strings::join(",", ROLE1, ROLE2);
  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL, UPDATED_WEIGHTS1);
}


// Checks that an update weight request is rejected
// for unauthenticated principals.
TEST_F(DynamicWeightsTest, UnauthenticatedUpdateWeightRequest)
{
  // The master is configured so that only requests from `DEFAULT_CREDENTIAL`
  // are authenticated.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Credential credential;
  credential.set_principal("unknown-principal");
  credential.set_secret("test-secret");

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response1 = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(credential),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response1);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);

  // The absence of credentials leads to authentication failure as well.
  infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response2 = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          None(),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response2);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// Checks that a weight query request is rejected for unauthenticated
// principals.
TEST_F(DynamicWeightsTest, UnauthenticatedQueryWeightRequest)
{
  // The master is configured so that only requests from `DEFAULT_CREDENTIAL`
  // are authenticated.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Credential credential;
  credential.set_principal("unknown-principal");
  credential.set_secret("test-secret");

  // Send a weight query request.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "GET",
          false,
          "weights",
          createBasicAuthHeaders(credential)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// Checks that an authorized principal can get weights.
TEST_F(DynamicWeightsTest, AuthorizedGetWeightsRequest)
{
  // Setup ACLs so that the default principal (DEFAULT_CREDENTIAL.principal())
  // can update weight for `ROLE1` and `ROLE2`.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_roles()->add_values(ROLE1);
  acl->mutable_roles()->add_values(ROLE2);

  // Setup ACLs so that default principal can only see ROLE1's weights and
  // default principal 2 can only see ROLE2's weights.
  mesos::ACL::ViewRole* getACL1 = acls.add_view_roles();
  getACL1->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  getACL1->mutable_roles()->add_values(ROLE1);

  mesos::ACL::ViewRole* getACL2 = acls.add_view_roles();
  getACL2->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
  getACL2->mutable_roles()->add_values(ROLE2);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL, GET_WEIGHTS1);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL_2, GET_WEIGHTS2);
}


// Checks that an authorized principal can update weight with implicit roles.
TEST_F(DynamicWeightsTest, AuthorizedWeightUpdateRequest)
{
  // Setup ACLs so that the default principal (DEFAULT_CREDENTIAL.principal())
  // can update weight for `ROLE1` and `ROLE2`.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_roles()->add_values(ROLE1);
  acl->mutable_roles()->add_values(ROLE2);

  // Ensure the get weights check pass under restrictive mode.
  mesos::ACL::ViewRole* getAcl = acls.add_view_roles();
  getAcl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  getAcl->mutable_roles()->add_values(ROLE1);
  getAcl->mutable_roles()->add_values(ROLE2);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL, UPDATED_WEIGHTS1);
}


// Checks that update weight requests can be authorized without authentication
// if an authorization rule exists that applies to anyone. The authorizer
// will map the absence of a principal to "ANY".
TEST_F(DynamicWeightsTest, AuthorizedUpdateWeightRequestWithoutPrincipal)
{
  // Setup ACLs so that any principal can update weight
  // for `ROLE1` and `ROLE2`.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_roles()->add_values(ROLE1);
  acl->mutable_roles()->add_values(ROLE2);

  // Ensure the get weights pass under restrictive mode.
  mesos::ACL::ViewRole* getAcl = acls.add_view_roles();
  getAcl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  getAcl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Disable authentication and set acls.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http_readwrite = false;
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          None(),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL, UPDATED_WEIGHTS1);
}


// Checks that an unauthorized principal cannot update weight.
TEST_F(DynamicWeightsTest, UnauthorizedWeightUpdateRequest)
{
  // Set ACLs to be non-permissive by default so that no principal can
  // update weight for `ROLE1` and `ROLE2`.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the roles in UPDATED_WEIGHTS1.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS1);
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  checkWithGetRequest(master.get()->pid, DEFAULT_CREDENTIAL);
}


// Checks that the weights information can be recovered from the registry.
TEST_F(DynamicWeightsTest, RecoveredWeightsFromRegistry)
{
  // Start a master with `--weights` flag.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";
  masterFlags.weights = UPDATED_WEIGHTS1;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Tests whether the weights stored in the registry are initialized
  // using the `--weights` flag when the cluster is bootstrapped.
  {
    checkWithGetRequest(
        master.get()->pid,
        DEFAULT_CREDENTIAL,
        UPDATED_WEIGHTS1);

    // Stop the master
    master->reset();

    // Restart the master.
    masterFlags.weights = None();
    master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    checkWithGetRequest(
        master.get()->pid,
        DEFAULT_CREDENTIAL,
        UPDATED_WEIGHTS1);
  }

  // Tests whether the weights stored in the registry are updated
  // successfully using the `/weights` endpoint.
  {
    // Send a weights update request for the specified roles.
    RepeatedPtrField<WeightInfo> infos = createWeightInfos(UPDATED_WEIGHTS2);
    Future<Response> response = process::http::request(
        process::http::createRequest(
            master.get()->pid,
            "PUT",
            false,
            "weights",
            createBasicAuthHeaders(DEFAULT_CREDENTIAL),
            strings::format("%s", JSON::protobuf(infos)).get()));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    checkWithGetRequest(
        master.get()->pid,
        DEFAULT_CREDENTIAL,
        UPDATED_WEIGHTS2);

    // Stop the master
    master->reset();

    // Restart the master without `--weights` flag.
    masterFlags.weights = None();
    master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    checkWithGetRequest(
        master.get()->pid,
        DEFAULT_CREDENTIAL,
        UPDATED_WEIGHTS2);
  }

  // Tests whether the `--weights` flag is ignored and use the registry value
  // instead when Mesos master subsequently starts with `--weights` flag
  // still specified.
  {
    // Stop the master
    master->reset();

    // Restart the master with `--weights` flag.
    masterFlags.weights = UPDATED_WEIGHTS1;
    master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    checkWithGetRequest(
        master.get()->pid,
        DEFAULT_CREDENTIAL,
        UPDATED_WEIGHTS2);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
