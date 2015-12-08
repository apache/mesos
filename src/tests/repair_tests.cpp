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
        "application/json",                                                \
        "Content-Type",                                                    \
      response);                                                           \
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);                \
    AWAIT_EXPECT_RESPONSE_BODY_EQ(jsonResponse, response);


TEST_F(HealthTest, ObserveEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Empty get to the observe endpoint.
  Future<Response> response = process::http::get(master.get(), "observe");
  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Empty post to the observe endpoint.
  response = process::http::post(master.get(), "observe");
  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Query string is ignored.
  response = process::http::post(master.get(), "observe?monitor=foo");
  VALIDATE_BAD_RESPONSE(response, "Missing value for 'monitor'");

  // Malformed value causes error.
  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=foo%");
  VALIDATE_BAD_RESPONSE(
      response,
      "Unable to decode query string: Malformed % escape in 'foo%': '%'");

  // Empty value causes error.
  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=");
  VALIDATE_BAD_RESPONSE(response, "Empty string for 'monitor'");

  // Missing hosts.
  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a");
  VALIDATE_BAD_RESPONSE(response, "Missing value for 'hosts'");

  // Missing level.
  response = process::http::post(
      master.get(),
      "observe",
      None(),
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
      None(),
      "monitor=a&hosts=b&level=ok");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // ok is case-insensitive.
  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a&hosts=b&level=Ok");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a&hosts=b&level=oK");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a&hosts=b&level=OK");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // level != OK  is unhealthy.
  expected.isHealthy = false;
  response =
    process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a&hosts=b&level=true");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  // Comma-separated hosts are parsed into an array.
  expected.hosts.push_back("e");
  response = process::http::post(
      master.get(),
      "observe",
      None(),
      "monitor=a&hosts=b,e&level=true");
  VALIDATE_GOOD_RESPONSE(response, stringify(expected));

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
