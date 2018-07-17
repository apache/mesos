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

#include <gtest/gtest.h>

#include <vector>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>
#include <mesos/authentication/http/combined_authenticator.hpp>

#include <mesos/module/http_authenticator.hpp>

#include <process/authenticator.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>

#include <stout/base64.hpp>

#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace process;

using std::string;
using std::vector;

namespace process {
namespace http {

bool operator==(const Forbidden &left, const Forbidden &right)
{
  return left.body == right.body;
}


bool operator==(const Unauthorized &left, const Unauthorized &right)
{
  return left.body == right.body &&
         left.headers.get("WWW-Authenticate") ==
             right.headers.get("WWW-Authenticate");
}


bool operator==(const URL &left, const URL &right)
{
  return left.scheme == right.scheme &&
         left.domain == right.domain &&
         left.ip == right.ip &&
         left.path == right.path &&
         left.query == right.query &&
         left.fragment == right.fragment &&
         left.port == right.port;
}


bool operator==(const Request &left, const Request &right)
{
  return left.headers == right.headers &&
         left.url == right.url &&
         left.method == right.method &&
         left.body == right.body;
}


namespace authentication {

bool operator==(
    const AuthenticationResult& left,
    const AuthenticationResult& right)
{
  return left.principal == right.principal &&
      left.unauthorized == right.unauthorized &&
      left.forbidden == right.forbidden;
}

} // namespace authentication {

} // namespace http {
} // namespace process {

namespace mesos {
namespace internal {
namespace tests {

using mesos::http::authentication::BasicAuthenticatorFactory;
using mesos::http::authentication::CombinedAuthenticator;

using process::http::Forbidden;
using process::http::Request;
using process::http::Unauthorized;

using process::http::authentication::Authenticator;
using process::http::authentication::AuthenticationResult;
using process::http::authentication::Principal;


static const string REALM = "tatooine";

static Parameters createBasicAuthenticatorParameters(
    const Option<string>& realm,
    const Option<Credentials>& credentials)
{
  Parameters parameters;

  if (realm.isSome()) {
    Parameter* parameter = parameters.add_parameter();
    parameter->set_key("authentication_realm");
    parameter->set_value(realm.get());
  }

  if (credentials.isSome()) {
    Parameter* parameter = parameters.add_parameter();
    parameter->set_key("credentials");
    parameter->set_value(
        stringify(JSON::protobuf(credentials->credentials())));
  }

  return parameters;
}

template <typename T>
class HttpAuthenticationTest : public MesosTest {};

typedef ::testing::Types<
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    tests::Module<Authenticator, TestHttpBasicAuthenticator>,
#endif // __WINDOWS__
    BasicAuthenticatorFactory> HttpAuthenticatorTypes;

TYPED_TEST_CASE(HttpAuthenticationTest, HttpAuthenticatorTypes);


// Tests the HTTP basic authenticator without credentials.
// Full HTTP stack tests are located in libprocess-tests.
TYPED_TEST(HttpAuthenticationTest, BasicWithoutCredentialsTest)
{
  Parameters parameters = createBasicAuthenticatorParameters(REALM, None());

  Try<Authenticator*> create = TypeParam::create(parameters);
  ASSERT_SOME(create);
  Owned<Authenticator> authenticator(create.get());

  EXPECT_EQ("Basic", authenticator->scheme());

  // No credentials given.
  {
    AuthenticationResult unauthorized;
    unauthorized.unauthorized =
      Unauthorized({"Basic realm=\"" + REALM + "\""});

    Request request;

    AWAIT_EXPECT_EQ(unauthorized, authenticator->authenticate(request));
  }

  // Unrecognized credentials.
  {
    Request request;
    request.headers.put(
        "Authorization",
        "Basic " + base64::encode("user:password"));

    AuthenticationResult unauthorized;
    unauthorized.unauthorized =
      Unauthorized({"Basic realm=\"" + REALM + "\""});

    AWAIT_EXPECT_EQ(unauthorized, authenticator->authenticate(request));
  }
}


// Tests the HTTP basic authenticator with credentials.
// Full HTTP stack tests are located in libprocess-tests.
TYPED_TEST(HttpAuthenticationTest, BasicWithCredentialsTest)
{
  Credentials credentials;
  Credential* credential = credentials.add_credentials();
  credential->set_principal("user");
  credential->set_secret("password");

  Parameters parameters =
    createBasicAuthenticatorParameters(REALM, credentials);

  Try<Authenticator*> create = TypeParam::create(parameters);
  ASSERT_SOME(create);
  Owned<Authenticator> authenticator(create.get());

  EXPECT_EQ("Basic", authenticator->scheme());

  // No credentials given.
  {
    Request request;

    AuthenticationResult unauthorized;
    unauthorized.unauthorized =
      Unauthorized({"Basic realm=\"" + REALM + "\""});

    AWAIT_EXPECT_EQ(unauthorized, authenticator->authenticate(request));
  }

  // Wrong credentials given.
  {
    Request request;

    request.headers.put(
        "Authorization",
        "Basic " + base64::encode("wronguser:wrongpassword"));

    AuthenticationResult unauthorized;
    unauthorized.unauthorized =
      Unauthorized({"Basic realm=\"" + REALM + "\""});

    AWAIT_EXPECT_EQ(unauthorized, authenticator->authenticate(request));
  }

  // Right credentials given.
  {
    Request request;

    AuthenticationResult result;
    result.principal = Principal("user");

    request.headers.put(
        "Authorization",
        "Basic " + base64::encode("user:password"));

    AWAIT_EXPECT_EQ(result, authenticator->authenticate(request));
  }
}


// Tests that the HTTP basic authenticator will return an error if it is given
// module parameters that don't contain an authentication realm.
TYPED_TEST(HttpAuthenticationTest, BasicWithoutRealm)
{
  Credentials credentials;
  Credential* credential = credentials.add_credentials();
  credential->set_principal("user");
  credential->set_secret("password");

  Parameters parameters =
    createBasicAuthenticatorParameters(None(), credentials);

  Try<Authenticator*> create = TypeParam::create(parameters);

  ASSERT_ERROR(create);
}


class MockAuthenticator : public Authenticator
{
public:
  MockAuthenticator(string scheme) : mockScheme(scheme) {}

  MockAuthenticator(const MockAuthenticator& authenticator)
    : mockScheme(authenticator.mockScheme) {}

  MOCK_METHOD1(authenticate, Future<AuthenticationResult>(const Request&));

  string scheme() const override { return mockScheme; }

private:
  const string mockScheme;
};


AuthenticationResult createUnauthorized(const MockAuthenticator& authenticator)
{
  AuthenticationResult result;
  result.unauthorized = Unauthorized(
      {authenticator.scheme() + " realm=\"" + REALM + "\""},
      authenticator.scheme() + " unauthorized");

  return result;
}


AuthenticationResult createForbidden(MockAuthenticator& authenticator)
{
  AuthenticationResult result;
  result.forbidden = Forbidden(authenticator.scheme() + " forbidden");

  return result;
}


AuthenticationResult createCombinedUnauthorized(
    const vector<MockAuthenticator>& authenticators)
{
  AuthenticationResult result;
  vector<string> headers;
  vector<string> bodies;

  foreach (const MockAuthenticator& authenticator, authenticators) {
    headers.push_back(authenticator.scheme() + " realm=\"" + REALM + "\"");
    bodies.push_back(
        "\"" + authenticator.scheme() + "\" authenticator returned:\n" +
        authenticator.scheme() + " unauthorized");
  }

  result.unauthorized = Unauthorized(
      {strings::join(",", headers)},
      strings::join("\n\n", bodies));

  return result;
}


AuthenticationResult createCombinedForbidden(
    const vector<MockAuthenticator>& authenticators)
{
  AuthenticationResult result;
  vector<string> bodies;

  foreach (const MockAuthenticator& authenticator, authenticators) {
    bodies.push_back(
        "\"" + authenticator.scheme() + "\" authenticator returned:\n" +
        authenticator.scheme() + " forbidden");
  }

  result.forbidden = Forbidden(strings::join("\n\n", bodies));

  return result;
}


// Verifies the functionality of the `CombinedAuthenticator`.
//
// Note: This test relies on the order of invocation of the installed
// authenticators. If the `CombinedAuthenticator` is changed in the future to
// call them in a different order, this test must be updated.
TEST(CombinedAuthenticatorTest, MultipleAuthenticators)
{
  // Create two mock HTTP authenticators to install.
  MockAuthenticator* basicAuthenticator = new MockAuthenticator("Basic");
  MockAuthenticator* bearerAuthenticator = new MockAuthenticator("Bearer");

  // Create a `CombinedAuthenticator` containing multiple authenticators.
  Owned<Authenticator> combinedAuthenticator(
      new CombinedAuthenticator(
          REALM,
          {
            Owned<Authenticator>(basicAuthenticator),
            Owned<Authenticator>(bearerAuthenticator)
          }
      ));

  Request request;
  request.headers.put(
      "Authorization",
      "Basic " + base64::encode("user:password"));

  // The first authenticator succeeds.
  {
    AuthenticationResult successfulResult;
    successfulResult.principal = Principal("user");

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(successfulResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(successfulResult, result);
  }

  // The first authenticator fails but the second one succeeds.
  {
    AuthenticationResult successfulResult;
    successfulResult.principal = Principal("user");

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*basicAuthenticator)));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(successfulResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(successfulResult, result);
  }

  // Two Unauthorized results.
  {
    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*basicAuthenticator)));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(
        createCombinedUnauthorized({*basicAuthenticator, *bearerAuthenticator}),
        result);
  }

  // One Unauthorized and one Forbidden result.
  {
    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*basicAuthenticator)));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createForbidden(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(createCombinedUnauthorized({*basicAuthenticator}), result);
  }

  // Two Forbidden results.
  {
    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(createForbidden(*basicAuthenticator)));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createForbidden(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(
        createCombinedForbidden({*basicAuthenticator, *bearerAuthenticator}),
        result);
  }

  // Two empty results.
  {
    AuthenticationResult emptyResult;

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(emptyResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(emptyResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(emptyResult, result);
  }

  // One empty and one Unauthorized result.
  {
    AuthenticationResult emptyResult;

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(emptyResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(createCombinedUnauthorized({*bearerAuthenticator}), result);
  }

  // One empty and one successful result.
  {
    AuthenticationResult emptyResult;
    AuthenticationResult successfulResult;
    successfulResult.principal = Principal("user");

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(emptyResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(successfulResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(successfulResult, result);
  }

  // Two failed futures.
  {
    Future<AuthenticationResult> failedResult(Failure("Failed result"));

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(failedResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(failedResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_FAILED(result);
  }

  // One failed future and one Unauthorized result.
  {
    Future<AuthenticationResult> failedResult(Failure("Failed result"));

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(failedResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createUnauthorized(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(createCombinedUnauthorized({*bearerAuthenticator}), result);
  }

  // One failed future and one Forbidden result.
  {
    Future<AuthenticationResult> failedResult(Failure("Failed result"));

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(failedResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(createForbidden(*bearerAuthenticator)));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(createCombinedForbidden({*bearerAuthenticator}), result);
  }

  // One failed future and one successful result.
  {
    Future<AuthenticationResult> failedResult(Failure("Failed result"));
    AuthenticationResult successfulResult;
    successfulResult.principal = Principal("user");

    EXPECT_CALL(*basicAuthenticator, authenticate(request))
      .WillOnce(Return(failedResult));
    EXPECT_CALL(*bearerAuthenticator, authenticate(request))
      .WillOnce(Return(successfulResult));

    Future<AuthenticationResult> result =
      combinedAuthenticator->authenticate(request);
    AWAIT_EXPECT_EQ(successfulResult, result);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
