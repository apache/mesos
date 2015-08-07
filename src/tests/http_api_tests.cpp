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

#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/recordio.hpp>

#include "common/http.hpp"
#include "common/recordio_response.hpp"

#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"


using mesos::internal::master::Master;

using mesos::internal::RecordIOResponseReader;

using mesos::scheduler::Call;
using mesos::scheduler::Event;

using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::OK;
using process::http::Pipe;
using process::http::Response;
using process::http::UnsupportedMediaType;

using std::string;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {


class HttpApiTest
  : public MesosTest,
    public WithParamInterface<string>
{
public:
  Try<Event> deserialize(
      const std::string& contentType,
      const std::string& body)
  {
    if (contentType == APPLICATION_PROTOBUF) {
      Event event;
      if (!event.ParseFromString(body)) {
        return Error("Failed to parse body into Event protobuf");
      }
      return event;
    }

    Try<JSON::Value> value = JSON::parse(body);
    Try<Event> parse = ::protobuf::parse<Event>(value.get());
    return parse;
  }

  std::string serialize(const Call& call, const std::string& contentType)
  {
    if (contentType == APPLICATION_PROTOBUF) {
      return call.SerializeAsString();
    }

    return stringify(JSON::Protobuf(call));
  }
};


// The HttpApi tests are parameterized by the content type.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    HttpApiTest,
    ::testing::Values(APPLICATION_PROTOBUF, APPLICATION_JSON));


// TODO(anand): Add tests for:
// - A subscribed scheduler closes it's reader and then tries to
//  subscribe again before the framework failover timeout and should
//  succeed.
//
// - A subscribed PID scheduler disconnects and then tries to
//  subscribe again as a HTTP framework before the framework failover
//  timeout and should succeed.


// TODO(anand): Add additional tests for validation.
TEST_F(HttpApiTest, NoContentType)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Expect a BadRequest when 'Content-Type' is omitted.
  //
  // TODO(anand): Send a valid call here to ensure that
  // the BadRequest is only due to the missing header.
  Future<Response> response = process::http::post(
      master.get(),
      "call",
      None(),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(HttpApiTest, ValidJsonButInvalidProtobuf)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  JSON::Object object;
  object.values["string"] = "valid_json";

  hashmap<string, string> headers;
  headers["Accept"] = APPLICATION_JSON;

  Future<Response> response = process::http::post(
      master.get(),
      "call",
      headers,
      stringify(object),
      APPLICATION_JSON);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a malformed body that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_P(HttpApiTest, MalformedContent)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  const std::string body = "MALFORMED_CONTENT";

  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::post(
      master.get(),
      "call",
      headers,
      body,
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sets an unsupported media type as Content-Type. This
// should result in a 415 (UnsupportedMediaType) response.
TEST_P(HttpApiTest, UnsupportedContentMediaType)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_FRAMEWORK_INFO);

  const std::string unknownMediaType = "application/unknown-media-type";

  Future<Response> response = process::http::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      unknownMediaType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);
}


// This test verifies if the scheduler is able to receive a Subscribed
// event on the stream in response to a Subscribe call request.
TEST_P(HttpApiTest, Subscribe)
{
  // TODO(anand): Enable authentication later.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

  // Retrieve the parameter passed as content type to this test.
  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(Response::PIPE, response.get().type);

  Option<Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer =
    lambda::bind(&HttpApiTest::deserialize, this, contentType, lambda::_1);

  recordio::Decoder<Event> decoder(deserializer);
  RecordIOResponseReader<Event>
    responseDecoder(decoder, reader.get());

  Future<Option<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(event.get().get().type(), Event::SUBSCRIBED);
  EXPECT_NE(event.get().get().subscribed().framework_id().value(), "");

  Shutdown();
}


// This test verifies if the scheduler can subscribe on retrying,
// e.g. after a ZK blip.
TEST_P(HttpApiTest, SubscribedOnRetryWithForce)
{
  // TODO(anand): Enable authentication later.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

  // Retrieve the parameter passed as content type to this test.
  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  ASSERT_EQ(Response::PIPE, response.get().type);

  Option<Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer =
    lambda::bind(&HttpApiTest::deserialize, this, contentType, lambda::_1);

  recordio::Decoder<Event> decoder(deserializer);
  RecordIOResponseReader<Event>
    responseDecoder(decoder, reader.get());

  Future<Option<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(event.get().get().type(), Event::SUBSCRIBED);
  EXPECT_NE(event.get().get().subscribed().framework_id().value(), "");

  // Now subscribe again with force set.
  subscribe->set_force(true);

  call.mutable_framework_id()->
    CopyFrom(event.get().get().subscribed().framework_id());

  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(event.get().get().subscribed().framework_id());

  auto response2 = process::http::streaming::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      contentType);

  Option<Pipe::Reader> reader2 = response2.get().reader;
  ASSERT_SOME(reader2);

  recordio::Decoder<Event> decoder2(deserializer);
  RecordIOResponseReader<Event>
    responseDecoder2(decoder2, reader2.get());

  // Check if we were successfully able to subscribe after the blip.
  Future<Option<Event>> event2 = responseDecoder2.read();
  AWAIT_READY(event2);
  ASSERT_FALSE(event2.isFailed());
  ASSERT_SOME(event2.get());

  // Check event type is subscribed and the same framework id is set.
  ASSERT_EQ(event2.get().get().type(), Event::SUBSCRIBED);
  EXPECT_EQ(event2.get().get().subscribed().framework_id(),
            event.get().get().subscribed().framework_id());

  Shutdown();
}


// This test verifies if we are able to upgrade from a pid
// based framework to http when force is set.
TEST_P(HttpApiTest, UpdatePidToHttpScheduler)
{
  // TODO(anand): Enable authentication later.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler without credentials.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Check that driver is notified with an error when the http
  // framework is connected.
  Future<FrameworkErrorMessage> errorMessage =
    FUTURE_PROTOBUF(FrameworkErrorMessage(), _, _);

  EXPECT_CALL(sched, error(_, _));

  driver.start();

  AWAIT_READY(frameworkId);
  EXPECT_NE("", frameworkId.get().value());

  // Now try to subscribe as a HTTP framework.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(frameworkId.get());

  Call::Subscribe* subscribe = call.mutable_subscribe();

  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);
  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(frameworkId.get());

  subscribe->set_force(true);

  // Retrieve the parameter passed as content type to this test.
  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(Response::PIPE, response.get().type);

  Option<Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer =
    lambda::bind(&HttpApiTest::deserialize, this, contentType, lambda::_1);

  recordio::Decoder<Event> decoder(deserializer);
  RecordIOResponseReader<Event>
    responseDecoder(decoder, reader.get());

  Future<Option<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(event.get().get().type(), Event::SUBSCRIBED);
  EXPECT_EQ(frameworkId.get(),
            event.get().get().subscribed().framework_id());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that updating a PID based framework to HTTP
// framework fails when force is not set and the PID based
// framework is already connected.
TEST_P(HttpApiTest, UpdatePidToHttpSchedulerWithoutForce)
{
  // TODO(anand): Enable authentication later.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler without credentials.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);
  EXPECT_NE("", frameworkId.get().value());

  // Now try to subscribe using a HTTP framework without setting
  // 'force' field.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(frameworkId.get());

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);
  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(frameworkId.get());

  // Retrieve the parameter passed as content type to this test.
  const std::string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "call",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(Response::PIPE, response.get().type);

  Option<Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer =
    lambda::bind(&HttpApiTest::deserialize, this, contentType, lambda::_1);

  recordio::Decoder<Event> decoder(deserializer);
  RecordIOResponseReader<Event>
    responseDecoder(decoder, reader.get());

  Future<Option<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // We should be receiving an error event since the PID framework
  // was already connected.
  ASSERT_EQ(event.get().get().type(), Event::ERROR);

  driver.stop();
  driver.join();

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
