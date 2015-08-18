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

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/recordio.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "master/constants.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::DEFAULT_HEARTBEAT_INTERVAL;
using mesos::internal::master::Master;

using mesos::internal::recordio::Reader;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::PID;

using process::http::BadRequest;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Pipe;
using process::http::Response;
using process::http::Unauthorized;
using process::http::UnsupportedMediaType;

using recordio::Decoder;

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
  // TODO(anand): Use the serialize/deserialize from common/http.hpp
  // when they are available.
  Try<Event> deserialize(const string& contentType, const string& body)
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

  string serialize(const Call& call, const string& contentType)
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


TEST_F(HttpApiTest, AuthenticationRequired)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = true;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<Response> response = process::http::post(
      master.get(),
      "api/v1/scheduler",
      None(),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Unauthorized("Mesos master").status,
      response);
}


// TODO(anand): Add additional tests for validation.
TEST_F(HttpApiTest, NoContentType)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Expect a BadRequest when 'Content-Type' is omitted.
  //
  // TODO(anand): Send a valid call here to ensure that
  // the BadRequest is only due to the missing header.
  Future<Response> response = process::http::post(
      master.get(),
      "api/v1/scheduler",
      None(),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(HttpApiTest, ValidJsonButInvalidProtobuf)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  JSON::Object object;
  object.values["string"] = "valid_json";

  hashmap<string, string> headers;
  headers["Accept"] = APPLICATION_JSON;

  Future<Response> response = process::http::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      stringify(object),
      APPLICATION_JSON);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a malformed body that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_P(HttpApiTest, MalformedContent)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  const string body = "MALFORMED_CONTENT";

  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      body,
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sets an unsupported media type as Content-Type. This
// should result in a 415 (UnsupportedMediaType) response.
TEST_P(HttpApiTest, UnsupportedContentMediaType)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  const string unknownMediaType = "application/unknown-media-type";

  Future<Response> response = process::http::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      unknownMediaType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);
}


// This test verifies if the scheduler is able to receive a Subscribed
// event and heartbeat events on the stream in response to a Subscribe
// call request.
TEST_P(HttpApiTest, Subscribe)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
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

  Reader<Event> responseDecoder(Decoder<Event>(deserializer), reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(Event::SUBSCRIBED, event.get().get().type());
  EXPECT_NE("", event.get().get().subscribed().framework_id().value());

  // Make sure it receives a heartbeat.
  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event.get().get().type());

  // Advance the clock to receive another heartbeat.
  Clock::pause();
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);

  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event.get().get().type());

  Shutdown();
}


// This test verifies if the scheduler can subscribe on retrying,
// e.g. after a ZK blip.
TEST_P(HttpApiTest, SubscribedOnRetryWithForce)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  auto deserializer =
    lambda::bind(&HttpApiTest::deserialize, this, contentType, lambda::_1);

  v1::FrameworkID frameworkId;

  {
    Future<Response> response = process::http::streaming::post(
        master.get(),
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(Response::PIPE, response.get().type);

    Option<Pipe::Reader> reader = response.get().reader;
    ASSERT_SOME(reader);

    Reader<Event> responseDecoder(Decoder<Event>(deserializer), reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    frameworkId = event.get().get().subscribed().framework_id();

    // Check event type is subscribed and the framework id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event.get().get().type());
    EXPECT_NE("", event.get().get().subscribed().framework_id().value());
  }

  {
    // Now subscribe again with force set.
    subscribe->set_force(true);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    Future<Response> response = process::http::streaming::post(
        master.get(),
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    Option<Pipe::Reader> reader = response.get().reader;
    ASSERT_SOME(reader);

    Reader<Event> responseDecoder(Decoder<Event>(deserializer), reader.get());

    // Check if we were successfully able to subscribe after the blip.
    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the same framework id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event.get().get().type());
    EXPECT_EQ(frameworkId, event.get().get().subscribed().framework_id());

    // Make sure it receives a heartbeat.
    event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    ASSERT_EQ(Event::HEARTBEAT, event.get().get().type());
  }

  Shutdown();
}


// This test verifies if we are able to upgrade from a PID based
// framework to HTTP when force is set.
TEST_P(HttpApiTest, UpdatePidToHttpScheduler)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  v1::FrameworkInfo frameworkInfo = DEFAULT_V1_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Start the scheduler without credentials.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &detector, devolve(frameworkInfo));

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

  // Now try to subscribe as an HTTP framework.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));

  Call::Subscribe* subscribe = call.mutable_subscribe();

  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);
  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(evolve(frameworkId.get()));

  subscribe->set_force(true);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
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

  Reader<Event> responseDecoder(Decoder<Event>(deserializer), reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(Event::SUBSCRIBED, event.get().get().type());
  EXPECT_EQ(evolve(frameworkId.get()),
            event.get().get().subscribed().framework_id());

  // Make sure it receives a heartbeat.
  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event.get().get().type());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that updating a PID based framework to HTTP
// framework fails when force is not set and the PID based
// framework is already connected.
TEST_P(HttpApiTest, UpdatePidToHttpSchedulerWithoutForce)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  v1::FrameworkInfo frameworkInfo = DEFAULT_V1_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Start the scheduler without credentials.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get());
  TestingMesosSchedulerDriver driver(&sched, &detector, devolve(frameworkInfo));

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);
  EXPECT_NE("", frameworkId.get().value());

  // Now try to subscribe using a HTTP framework without setting the
  // 'force' field.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);
  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(evolve(frameworkId.get()));

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();
  hashmap<string, string> headers;
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
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

  Reader<Event> responseDecoder(Decoder<Event>(deserializer), reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // We should be receiving an error event since the PID framework
  // was already connected.
  ASSERT_EQ(Event::ERROR, event.get().get().type());

  // Unsubscribed HTTP framework should not get any heartbeats.
  Clock::pause();
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);
  Clock::settle();

  // The next read should be EOF.
  event = responseDecoder.read();
  AWAIT_READY(event);
  EXPECT_NONE(event.get());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_P(HttpApiTest, NotAcceptable)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  hashmap<string, string> headers;
  headers["Accept"] = "foo";

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(NotAcceptable().status, response);
}


TEST_P(HttpApiTest, NoAcceptHeader)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  // No 'Accept' header leads to all media types considered
  // acceptable. JSON will be chosen by default.
  hashmap<string, string> headers;

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ(APPLICATION_JSON, response.get().headers.get("Content-Type"));
}


TEST_P(HttpApiTest, DefaultAccept)
{
  // HTTP schedulers cannot yet authenticate.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  hashmap<string, string> headers;
  headers["Accept"] = "*/*";

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  Future<Response> response = process::http::streaming::post(
      master.get(),
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ(APPLICATION_JSON, response.get().headers.get("Content-Type"));
}


TEST_F(HttpApiTest, GetRequest)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get(),
      "api/v1/scheduler");

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(MethodNotAllowed().status, response);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
