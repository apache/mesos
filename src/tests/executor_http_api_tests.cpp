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

#include <mesos/v1/executor/executor.hpp>

#include <mesos/http.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include "common/http.hpp"

#include "master/master.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::v1::executor::Call;

using process::Clock;
using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Response;
using process::http::UnsupportedMediaType;

using std::string;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {


class ExecutorHttpApiTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// The tests are parameterized by the content type of the request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    ExecutorHttpApiTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// TODO(anand): Add more validation tests for:
// - If the slave is still recovering, the call should return
//   ServiceUnavailable.
// - If Executor is not found, the call should return
//   BadRequest.
// - If Executor has not registered and sends a Call message other
//   than Subscribe, the call should return Forbidden.


// This test expects a BadRequest when 'Content-Type' is omitted.
TEST_F(ExecutorHttpApiTest, NoContentType)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  Call call;
  call.set_type(Call::MESSAGE);

  call.mutable_message()->set_data("hello world");
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  Future<Response> response = process::http::post(
      slave.get(),
      "api/v1/executor",
      None(),
      serialize(ContentType::JSON, call),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(ExecutorHttpApiTest, ValidJsonButInvalidProtobuf)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  JSON::Object object;
  object.values["string"] = "valid_json";

  hashmap<string, string> headers;
  headers["Accept"] = APPLICATION_JSON;

  Future<Response> response = process::http::post(
      slave.get(),
      "api/v1/executor",
      headers,
      stringify(object),
      APPLICATION_JSON);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This test sends a malformed body that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_P(ExecutorHttpApiTest, MalformedContent)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  const string body = "MALFORMED_CONTENT";

  const ContentType contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = stringify(contentType);

  Future<Response> response = process::http::post(
      slave.get(),
      "api/v1/executor",
      headers,
      body,
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This test sets an unsupported media type as Content-Type. This
// should result in a 415 (UnsupportedMediaType) response.
TEST_P(ExecutorHttpApiTest, UnsupportedContentMediaType)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  ContentType contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = stringify(contentType);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  const string unknownMediaType = "application/unknown-media-type";

  Future<Response> response = process::http::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      unknownMediaType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);

  Shutdown();
}


// This test sends a Call from an unknown FrameworkID. The call
// should return a BadRequest.
TEST_P(ExecutorHttpApiTest, MessageFromUnknownFramework)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  ContentType contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = stringify(contentType);

  Call call;
  call.set_type(Call::MESSAGE);

  call.mutable_message()->set_data("hello world");
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  Future<Response> response = process::http::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  Shutdown();
}


// This test sends a GET request to the executor HTTP endpoint instead
// of a POST. The call should return a MethodNotAllowed response.
TEST_F(ExecutorHttpApiTest, GetRequest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  Future<Response> response = process::http::get(
      slave.get(),
      "api/v1/executor");

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(MethodNotAllowed().status, response);

  Shutdown();
}


// This test sends in a Accept:*/* header meaning it would Accept any
// media type as response. We return the default "application/json"
// media type as response.
TEST_P(ExecutorHttpApiTest, DefaultAccept)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  hashmap<string, string> headers;
  headers["Accept"] = "*/*";

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  Future<Response> response = process::http::streaming::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ(APPLICATION_JSON, response.get().headers.get("Content-Type"));

  Shutdown();
}


// This test does not set any Accept header for the subscribe call.
// The default response media type should be "application/json" in
// this case.
TEST_P(ExecutorHttpApiTest, NoAcceptHeader)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  // No 'Accept' header leads to all media types considered
  // acceptable. JSON will be chosen by default.
  hashmap<string, string> headers;

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  Future<Response> response = process::http::streaming::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ(APPLICATION_JSON, response.get().headers.get("Content-Type"));

  Shutdown();
}


// This test sends a unsupported Accept media type for the Accept
// header. The response should be NotAcceptable in this case.
TEST_P(ExecutorHttpApiTest, NotAcceptable)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  hashmap<string, string> headers;
  headers["Accept"] = "foo";

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  Future<Response> response = process::http::streaming::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(NotAcceptable().status, response);

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
