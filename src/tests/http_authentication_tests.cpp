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

#include <mesos/module/http_authenticator.hpp>

#include <process/authenticator.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>

#include <stout/base64.hpp>

#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace process;

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

using process::http::Request;
using process::http::Unauthorized;

using process::http::authentication::Authenticator;
using process::http::authentication::Principal;
using process::http::authentication::AuthenticationResult;


static const std::string REALM = "tatooine";

static Parameters createBasicAuthenticatorParameters(
    const Option<std::string>& realm,
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
