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

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "master/detector/standalone.hpp"

#include "tests/mesos.hpp"
#include "tests/module.hpp"

namespace http = process::http;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::StandaloneMasterDetector;

using process::Future;
using process::Owned;

using process::http::Forbidden;
using process::http::OK;
using process::http::Response;

using std::string;

using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

template <typename T>
class SlaveAuthorizerTest : public MesosTest {};


typedef ::testing::Types<
  LocalAuthorizer,
  tests::Module<Authorizer, TestLocalAuthorizer>>
  AuthorizerTypes;


TYPED_TEST_CASE(SlaveAuthorizerTest, AuthorizerTypes);


// This test verifies that only authorized principals
// can access the '/flags' endpoint.
TYPED_TEST(SlaveAuthorizerTest, AuthorizeFlagsEndpoint)
{
  const string endpoint = "flags";

  // Setup ACLs so that only the default principal
  // can access the '/flags' endpoint.
  ACLs acls;
  acls.set_permissive(false);

  mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_paths()->add_values("/" + endpoint);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> agent =
    this->StartSlave(&detector, authorizer.get());
  ASSERT_SOME(agent);

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;

  response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
    << response.get().body;
}


// This test verifies that access to the '/flags' endpoint can be authorized
// without authentication if an authorization rule exists that applies to
// anyone. The authorizer will map the absence of a principal to "ANY".
TYPED_TEST(SlaveAuthorizerTest, AuthorizeFlagsEndpointWithoutPrincipal)
{
  const string endpoint = "flags";

  // Because the authenticators' lifetime is tied to libprocess's lifetime,
  // it may already be set by other tests. We have to unset it here to disable
  // HTTP authentication.
  // TODO(nfnt): Fix this behavior. The authenticator should be unset by
  // every test case that sets it, similar to how it's done for the master.
  http::authentication::unsetAuthenticator(
      slave::DEFAULT_HTTP_AUTHENTICATION_REALM);

  // Setup ACLs so that any principal can access the '/flags' endpoint.
  ACLs acls;
  acls.set_permissive(false);

  mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_paths()->add_values("/" + endpoint);

  slave::Flags agentFlags = this->CreateSlaveFlags();
  agentFlags.acls = acls;
  agentFlags.authenticate_http = false;
  agentFlags.http_credentials = None();

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  StandaloneMasterDetector detector;
  Try<Owned<cluster::Slave>> agent = this->StartSlave(
      &detector, authorizer.get(), agentFlags);
  ASSERT_SOME(agent);

  Future<Response> response = http::get(agent.get()->pid, endpoint);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;
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
        "flags",
        "containers"));


// Tests that an agent endpoint handler forms
// correct queries against the authorizer.
TEST_P(SlaveEndpointTest, AuthorizedRequest)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  MockAuthorizer mockAuthorizer;

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, &mockAuthorizer);
  ASSERT_SOME(agent);

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
  EXPECT_EQ(principal, request.get().subject().value());

  // TODO(bbannier): Once agent endpoint handlers use more than just
  // `GET_ENDPOINT_WITH_PATH` we should factor out the request method
  // and expected authorization action and parameterize
  // `SlaveEndpointTest` on that as well in addition to the endpoint.
  EXPECT_EQ(authorization::GET_ENDPOINT_WITH_PATH, request.get().action());

  EXPECT_EQ("/" + endpoint, request.get().object().value());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;
}


// Tests that unauthorized requests for an agent endpoint are properly rejected.
TEST_P(SlaveEndpointTest, UnauthorizedRequest)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  MockAuthorizer mockAuthorizer;

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, &mockAuthorizer);
  ASSERT_SOME(agent);

  EXPECT_CALL(mockAuthorizer, authorized(_))
    .WillOnce(Return(false));

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
    << response.get().body;
}


// Tests that requests for an agent endpoint
// always succeed if the authorizer is absent.
TEST_P(SlaveEndpointTest, NoAuthorizer)
{
  const string endpoint = GetParam();

  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, CreateSlaveFlags());
  ASSERT_SOME(agent);

  Future<Response> response = http::get(
      agent.get()->pid,
      endpoint,
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response.get().body;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
