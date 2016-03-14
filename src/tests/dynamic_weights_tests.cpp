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

  // Create WeightInfos from the specified weights flag.
  RepeatedPtrField<WeightInfo> createWeightInfos(const string& weightsFlag)
  {
    RepeatedPtrField<WeightInfo> infos;
    vector<string> tokens = strings::tokenize(weightsFlag, ",");
    foreach (const string& token, tokens) {
      vector<string> pair = strings::tokenize(token, "=");
      EXPECT_EQ(2u, pair.size());
      double weight = atof(pair[1].c_str());
      WeightInfo weightInfo;
      weightInfo.set_role(pair[0]);
      weightInfo.set_weight(weight);
      infos.Add()->CopyFrom(weightInfo);
    }

    return infos;
  }

  // Generates a weights update request from the specified weights.
  string createUpdateRequestBody(
      const RepeatedPtrField<WeightInfo>& infos) const
  {
    const string request = strings::format(
        "%s",
        JSON::protobuf(infos)).get();

    return request;
  }

  void checkWithRolesEndpoint(
      const Try<PID<Master>>& master,
      const Option<string>& weights = None())
  {
    Future<Response> response = process::http::request(
        process::http::createRequest(
            master.get(),
            "GET",
            false,
            "roles",
            createBasicAuthHeaders(DEFAULT_CREDENTIAL)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> parse = JSON::parse(response.get().body);
    ASSERT_SOME(parse);

    Try<JSON::Value> expected = JSON::Null();

    if (weights.isNone()) {
      expected = JSON::parse(
          "{"
          "  \"roles\": ["
          "    {"
          "      \"frameworks\": [],"
          "      \"name\": \"*\","
          "      \"resources\": {"
          "        \"cpus\": 0,"
          "        \"disk\": 0,"
          "        \"mem\":  0"
          "      },"
          "      \"weight\": 1.0"
          "    }"
          "  ]"
          "}");
    } else if (weights == DEFAULT_WEIGHTS) {
      expected = JSON::parse(
          "{"
          "  \"roles\": ["
          "    {"
          "      \"frameworks\": [],"
          "      \"name\": \"*\","
          "      \"resources\": {"
          "        \"cpus\": 0,"
          "        \"disk\": 0,"
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
          "        \"mem\":  0"
          "      },"
          "      \"weight\": 1.0"
          "    },"
          "    {"
          "      \"frameworks\": [],"
          "      \"name\": \"role2\","
          "      \"resources\": {"
          "        \"cpus\": 0,"
          "        \"disk\": 0,"
          "        \"mem\":  0"
          "      },"
          "      \"weight\": 1.0"
          "    }"
          "  ]"
          "}");
    } else if (weights == UPDATED_WEIGHTS) {
      expected = JSON::parse(
          "{"
          "  \"roles\": ["
          "    {"
          "      \"frameworks\": [],"
          "      \"name\": \"*\","
          "      \"resources\": {"
          "        \"cpus\": 0,"
          "        \"disk\": 0,"
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
          "        \"mem\":  0"
          "      },"
          "      \"weight\": 2.0"
          "    },"
          "    {"
          "      \"frameworks\": [],"
          "      \"name\": \"role2\","
          "      \"resources\": {"
          "        \"cpus\": 0,"
          "        \"disk\": 0,"
          "        \"mem\":  0"
          "      },"
          "      \"weight\": 4.0"
          "    }"
          "  ]"
          "}");
    } else {
      expected = Error("Unexpected weights string.");
    }

    ASSERT_SOME(expected);
    EXPECT_EQ(expected.get(), parse.get());
  }

protected:
  const string ROLE1 = "role1";
  const string ROLE2 = "role2";
  const string DEFAULT_WEIGHTS = "role1=1.0,role2=1.0";
  const string UPDATED_WEIGHTS = "role1=2.0,role2=4.0";
};


// Update weights requests with invalid JSON structure
// should return a '400 Bad Request'.
TEST_F(DynamicWeightsTest, PutInvalidRequest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Tests whether an update weights request with invalid JSON fails.
  string badRequest =
    "[{"
    "  \"weight\":3.2,"
    "  \"role\""
    "}]";

  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          badRequest));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  // Tests whether an update weights request with an invalid field fails.
  // In this case, the correct field name should be 'role'.
  badRequest =
    "[{"
    "  \"weight\":3.2,"
    "  \"invalidField\":\"role1\""
    "}]";

  response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          badRequest));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// A update weights request with zero value
// should return a '400 Bad Request'.
TEST_F(DynamicWeightsTest, ZeroWeight)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a weight update request to update the weight of 'role1' to 0.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos("role1=0"))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// A update weights request with negative value
// should return a '400 Bad Request'.
TEST_F(DynamicWeightsTest, NegativeWeight)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a weight update request to update the weight of 'role1' to -2.0.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos("role1=-2.0"))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// A update weights request with non-numeric value
// should return a '400 Bad Request'.
TEST_F(DynamicWeightsTest, NonNumericWeight)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a weight update request to update the weight of 'role1' to 'two'
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos("role1=two"))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// Updates must be explicit about what role they are updating,
// and a missing role request should return a '400 Bad Request'.
TEST_F(DynamicWeightsTest, MissingRole)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Send a missing role update request.
  Future<Response> response1 = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          "weights=[{\"weight\":2.0}]"));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response1)
    << response1.get().body;

  checkWithRolesEndpoint(master);

  // Send an empty role (only a space) update request.
  Future<Response> response2 = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos(" =2.0"))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response2)
    << response2.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// Verifies that an update request for an unknown role (not specified
// in --roles flag) is rejected when using the explicit roles (--roles flag
// is specified when master is started).
TEST_F(DynamicWeightsTest, UnknownRole)
{
  // Specify --roles whitelist when starting master.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.roles = strings::join(",", ROLE1, ROLE2);
  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Send a weight update request for 'unknown' role.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos("unknown=3.0"))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master, DEFAULT_WEIGHTS);

  ShutdownMasters();
}


// Verifies that a weights update request for a whitelisted role
// (contained in --roles flag) succeeds when using the explicit
// roles (--roles flag is specified when master is started).
TEST_F(DynamicWeightsTest, UpdateWeightsWithExplictRoles)
{
  // Specify --roles whitelist when starting master.
  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.roles = strings::join(",", ROLE1, ROLE2);
  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  checkWithRolesEndpoint(master, DEFAULT_WEIGHTS);

  // Send a weight update request for the specified roles in UPDATED_WEIGHTS.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master, UPDATED_WEIGHTS);

  ShutdownMasters();
}


// Checks that an update weight request is rejected
// for unauthenticated principals.
TEST_F(DynamicWeightsTest, UnauthenticatedUpdateWeightRequest)
{
  // The master is configured so that only requests from `DEFAULT_CREDENTIAL`
  // are authenticated.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Credential credential;
  credential.set_principal("unknown-principal");
  credential.set_secret("test-secret");

  // Send a weight update request for the specified roles in UPDATED_WEIGHTS.
  Future<Response> response1 = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(credential),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response1)
    << response1.get().body;

  checkWithRolesEndpoint(master);

  // The absence of credentials leads to authentication failure as well.
  Future<Response> response2 = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          None(),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response2)
    << response2.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}


// Checks that an authorized principal can update weight with implicit roles.
TEST_F(DynamicWeightsTest, AuthorizedWeightUpdateRequest)
{
  // Setup ACLs so that the default principal (DEFAULT_CREDENTIAL.principal())
  // can update weight for `ROLE1` and `ROLE2`.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::UpdateWeights* acl = acls.add_update_weights();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_roles()->add_values(ROLE1);
  acl->mutable_roles()->add_values(ROLE2);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the specified roles in UPDATED_WEIGHTS.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master, UPDATED_WEIGHTS);

  ShutdownMasters();
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
  mesos::ACL::UpdateWeights* acl = acls.add_update_weights();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_roles()->add_values(ROLE1);
  acl->mutable_roles()->add_values(ROLE2);

  // Disable authentication and set acls.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http = false;
  masterFlags.acls = acls;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the specified roles in UPDATED_WEIGHTS.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          None(),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master, UPDATED_WEIGHTS);

  ShutdownMasters();
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

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Send a weight update request for the specified roles in UPDATED_WEIGHTS.
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get(),
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          createUpdateRequestBody(createWeightInfos(UPDATED_WEIGHTS))));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
    << response.get().body;

  checkWithRolesEndpoint(master);

  ShutdownMasters();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
