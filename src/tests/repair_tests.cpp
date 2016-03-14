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

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/json.hpp>

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using std::string;
using std::vector;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {


class HealthTest : public MesosTest {};


struct JsonResponse
{
  string monitor;
  vector<string> hosts;
  bool isHealthy;
};


string stringify(const JsonResponse& response)
{
  JSON::Object object;
  object.values["monitor"] = response.monitor;

  JSON::Array hosts;
  hosts.values.assign(response.hosts.begin(), response.hosts.end());
  object.values["hosts"] = hosts;

  object.values["isHealthy"] = response.isHealthy;

  return ::stringify(object);
}


// Using macros instead of a helper function so that we get good line
// numbers from the test run.
#define VALIDATE_BAD_RESPONSE(response, error)                             \
    AWAIT_READY(response);                                                 \
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);        \
    AWAIT_EXPECT_RESPONSE_BODY_EQ(error, response)

#define VALIDATE_GOOD_RESPONSE(response, jsonResponse)                     \
    AWAIT_READY(response);                                                 \
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(                                       \
        APPLICATION_JSON,                                                  \
        "Content-Type",                                                    \
        response);                                                         \
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);                \
    AWAIT_EXPECT_RESPONSE_BODY_EQ(jsonResponse, response);


TEST_F(HealthTest, ObserveEndpoint)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Empty get to the observe endpoint.
  Future<Response> response = process::http::get(
      master.get(),
      "observe",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Empty post to the observe endpoint.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Query string is ignored.
  response = process::http::post(
      master.get(),
      "observe?monitor=foo",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Malformed value causes error.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=foo%");

  VALIDATE_BAD_RESPONSE(
      response,
      "Unable to decode query string: Malformed % escape in 'foo%': '%'");

  // Empty value causes error.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=");

  VALIDATE_BAD_RESPONSE(response, "Empty string for 'monitor'");

  // Missing hosts.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a");

  VALIDATE_BAD_RESPONSE(response, "Missing value for 'hosts'");

  // Missing level.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b");

  VALIDATE_BAD_RESPONSE(response, "Missing value for 'level'");

  // Good request is successful.
  JsonResponse expected;
  expected.monitor = "a";
  expected.hosts.push_back("b");
  expected.isHealthy = true;

  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b&level=ok");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // ok is case-insensitive.
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b&level=Ok");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b&level=oK");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b&level=OK");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // level != OK  is unhealthy.
  expected.isHealthy = false;
  response =
    process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b&level=true");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // Comma-separated hosts are parsed into an array.
  expected.hosts.push_back("e");
  response = process::http::post(
      master.get(),
      "observe",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      "monitor=a&hosts=b,e&level=true");

  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  Shutdown();
}


// Testing get without authentication and with bad credentials.
TEST_F(HealthTest, ObserveEndpointBadAuthentication)
{
  // Set up a master with authentication required.
  // Note that the default master test flags enable HTTP authentication.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Headers for POSTs to maintenance endpoints without authentication.
  process::http::Headers unauthenticatedHeaders;
  unauthenticatedHeaders["Content-Type"] = "application/json";

  // Bad credentials which should fail authentication.
  Credential badCredential;
  badCredential.set_principal("badPrincipal");
  badCredential.set_secret("badSecret");

  // Headers for POSTs to maintenance endpoints with bad authentication.
  process::http::Headers badAuthenticationHeaders;
  badAuthenticationHeaders = createBasicAuthHeaders(badCredential);
  badAuthenticationHeaders["Content-Type"] = "application/json";

  // Post to observe without authentication.
  Future<Response> response = process::http::post(
      master.get(),
      "observe",
      unauthenticatedHeaders,
      "monitor=a&hosts=b&level=Ok");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  // Get request without authentication.
  response = process::http::get(master.get(), "observe");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  // Post to observe with bad authentication.
  response = process::http::post(
      master.get(),
      "observe",
      badAuthenticationHeaders,
      "monitor=a&hosts=b&level=Ok");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  // Get request with bad authentication.
  response = process::http::get(
    master.get(),
    "observe",
    None(),
    createBasicAuthHeaders(badCredential));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
