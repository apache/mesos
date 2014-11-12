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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include "logging/logging.hpp"

using namespace mesos::internal;

using process::http::BadRequest;
using process::http::OK;
using process::http::Response;


TEST(LoggingTest, Toggle)
{
  process::PID<> pid;
  pid.id = "logging";
  pid.node = process::node();

  process::Future<Response> response = process::http::get(pid, "toggle");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  response = process::http::get(pid, "toggle", "level=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'duration=value' in query.\n",
      response);

  response = process::http::get(pid, "toggle", "duration=10secs");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'level=value' in query.\n",
      response);

  response = process::http::get(pid, "toggle", "duration=10secs");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'level=value' in query.\n",
      response);

  response = process::http::get(pid, "toggle", "level=-1&duration=10secs");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Invalid level '-1'.\n",
      response);

  response = process::http::get(pid, "toggle", "level=-1&duration=10secs");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Invalid level '-1'.\n",
      response);
}
