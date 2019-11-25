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

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/recordio.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "master/constants.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::DEFAULT_HEARTBEAT_INTERVAL;
using mesos::internal::master::Master;

using mesos::internal::recordio::Reader;

using mesos::master::detector::StandaloneMasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::Owned;
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


class SchedulerHttpApiTest
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

    return stringify(JSON::protobuf(call));
  }
};


// The HttpApi tests are parameterized by the content type.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    SchedulerHttpApiTest,
    ::testing::Values(APPLICATION_PROTOBUF, APPLICATION_JSON));


// TODO(anand): Add tests for:
// - A subscribed scheduler closes its reader and then tries to
//  subscribe again before the framework failover timeout and should
//  succeed.
//
// - A subscribed PID scheduler disconnects and then tries to
//  subscribe again as a HTTP framework before the framework failover
//  timeout and should succeed.


TEST_F(SchedulerHttpApiTest, AuthenticationRequired)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1/scheduler",
      None(),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}


// TODO(anand): Add additional tests for validation.
TEST_F(SchedulerHttpApiTest, NoContentType)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Expect a BadRequest when 'Content-Type' is omitted.
  //
  // TODO(anand): Send a valid call here to ensure that
  // the BadRequest is only due to the missing header.
  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1/scheduler",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(SchedulerHttpApiTest, ValidJsonButInvalidProtobuf)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  JSON::Object object;
  object.values["string"] = "valid_json";

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = APPLICATION_JSON;

  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      stringify(object),
      APPLICATION_JSON);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sends a malformed body that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_P(SchedulerHttpApiTest, MalformedContent)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string body = "MALFORMED_CONTENT";

  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      body,
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test sets an unsupported media type as Content-Type. This
// should result in a 415 (UnsupportedMediaType) response.
TEST_P(SchedulerHttpApiTest, UnsupportedContentMediaType)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  const string unknownMediaType = "application/unknown-media-type";

  Future<Response> response = process::http::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      unknownMediaType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);
}


// This test verifies if the scheduler is able to receive a Subscribed
// event and heartbeat events on the stream in response to a Subscribe
// call request.
TEST_P(SchedulerHttpApiTest, Subscribe)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response->type);
  ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
  EXPECT_NE("", response->headers.at("Mesos-Stream-Id"));

  Option<Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  Reader<Event> responseDecoder(deserializer, reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
  EXPECT_NE("", event->get().subscribed().framework_id().value());

  // Make sure it receives a heartbeat.
  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event->get().type());

  // Advance the clock to receive another heartbeat.
  Clock::pause();
  Clock::advance(DEFAULT_HEARTBEAT_INTERVAL);

  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event->get().type());
}


// This test verifies if the role is invalid in scheduler's framework message,
// the event is error on the stream in response to a Subscribe call request.
TEST_P(SchedulerHttpApiTest, RejectFrameworkWithInvalidRole)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  v1::FrameworkInfo framework = v1::DEFAULT_FRAMEWORK_INFO;
  // Set invalid role.
  framework.set_roles(0, "/test/test1");
  subscribe->mutable_framework_info()->CopyFrom(framework);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response->type);

  Option<Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  Reader<Event> responseDecoder(deserializer, reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is error.
  ASSERT_EQ(Event::ERROR, event->get().type());
}


// This test verifies that the client will receive a `BadRequest` response if it
// includes a stream ID header with a subscribe call.
TEST_P(SchedulerHttpApiTest, SubscribeWithStreamId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;
  headers["Mesos-Stream-Id"] = id::UUID::random().toString();

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
}


// This test verifies if the scheduler can subscribe on retrying,
// e.g. after a ZK blip.
TEST_P(SchedulerHttpApiTest, SubscribedOnRetry)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  v1::FrameworkID frameworkId;

  {
    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(Response::PIPE, response->type);

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    Reader<Event> responseDecoder(deserializer, reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    frameworkId = event->get().subscribed().framework_id();

    // Check event type is subscribed and the framework id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
    EXPECT_NE("", event->get().subscribed().framework_id().value());
  }

  {
    call.mutable_framework_id()->CopyFrom(frameworkId);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    Reader<Event> responseDecoder(deserializer, reader.get());

    // Check if we were successfully able to subscribe after the blip.
    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the same framework id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
    EXPECT_EQ(frameworkId, event->get().subscribed().framework_id());

    // Make sure it receives a heartbeat.
    event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    ASSERT_EQ(Event::HEARTBEAT, event->get().type());
  }
}


// This test verifies if we are able to upgrade from a PID based
// scheduler to HTTP scheduler.
TEST_P(SchedulerHttpApiTest, UpdatePidToHttpScheduler)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector, devolve(frameworkInfo));

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Check that driver is notified with an error when the http
  // framework is connected.
  Future<FrameworkErrorMessage> errorMessage =
    FUTURE_PROTOBUF(FrameworkErrorMessage(), _, _);

  Future<Nothing> schedError;
  EXPECT_CALL(sched, error(_, _))
    .WillOnce(FutureSatisfy(&schedError));

  driver.start();

  AWAIT_READY(frameworkId);
  EXPECT_NE("", frameworkId->value());

  // Now try to subscribe as an HTTP framework.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));

  Call::Subscribe* subscribe = call.mutable_subscribe();

  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);
  subscribe->mutable_framework_info()->mutable_id()->
    CopyFrom(evolve(frameworkId.get()));

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response->type);

  Option<Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  Reader<Event> responseDecoder(deserializer, reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
  EXPECT_EQ(evolve(frameworkId.get()),
            event->get().subscribed().framework_id());

  // Make sure it receives a heartbeat.
  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event->get().type());

  AWAIT_READY(errorMessage);
  AWAIT_READY(schedError);

  driver.stop();
  driver.join();
}


// This test verifies that we are able to downgrade from a HTTP based
// framework to PID.
TEST_P(SchedulerHttpApiTest, UpdateHttpToPidScheduler)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response->type);

  Option<Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  Reader<Event> responseDecoder(deserializer, reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the framework id is set.
  ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
  frameworkInfo.mutable_id()->
    CopyFrom(event->get().subscribed().framework_id());

  // Make sure it receives a heartbeat.
  event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  ASSERT_EQ(Event::HEARTBEAT, event->get().type());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, devolve(frameworkInfo), master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);
  ASSERT_EQ(evolve(frameworkId.get()), frameworkInfo.id());

  driver.stop();
  driver.join();
}


// This test verifies that we are able to upgrade from a PID based
// framework to HTTP framework and then downgrade back.
TEST_P(SchedulerHttpApiTest, UpdateHttpToPidSchedulerAndBack)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Start a PID based scheduler instance first.
  MockScheduler scheduler;
  MesosSchedulerDriver driver(
      &scheduler,
      devolve(frameworkInfo),
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(scheduler, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);
  ASSERT_NE("", frameworkId->value());

  frameworkInfo.mutable_id()->CopyFrom(evolve(frameworkId.get()));

  // Expect "Framework failed over" message.
  EXPECT_CALL(scheduler, error(&driver, _));

  // Fail over to an HTTP based scheduler instance.
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(frameworkInfo.id());
  call.mutable_subscribe()->mutable_framework_info()->CopyFrom(frameworkInfo);

  // Retrieve content type passed as a parameter to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(Response::PIPE, response->type);

  Option<Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = lambda::bind(
      &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

  Reader<Event> responseDecoder(deserializer, reader.get());

  // Get SUBSCRIBED event and check framework ID.
  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());
  ASSERT_EQ(Event::SUBSCRIBED, event.get()->type());
  ASSERT_EQ(frameworkInfo.id(), event.get()->subscribed().framework_id());

  driver.stop();
  driver.join();

  // Fail over back to a PID based scheduler instance.
  MockScheduler scheduler2;
  MesosSchedulerDriver driver2(
      &scheduler2,
      devolve(frameworkInfo),
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(scheduler2, registered(&driver2, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver2.start();

  AWAIT_READY(frameworkId);
  ASSERT_EQ(devolve(frameworkInfo.id()), frameworkId.get());

  TaskStatus status;
  status.mutable_task_id()->set_value("task-1");

  Future<TaskStatus> reconciledStatus;
  EXPECT_CALL(scheduler2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&reconciledStatus));

  // Reconcile a non-existing task to exercise message handling. The
  // master used to crash when processing a message from a framework
  // that upgraded to an HTTP based driver and downgraded back to a
  // PID based driver, due to missing metrics.
  driver2.reconcileTasks({status});

  AWAIT_READY(reconciledStatus);

  driver2.stop();
  driver2.join();
}


TEST_P(SchedulerHttpApiTest, NotAcceptable)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = "foo";

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(NotAcceptable().status, response);
}


TEST_P(SchedulerHttpApiTest, NoAcceptHeader)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  // No 'Accept' header leads to all media types considered
  // acceptable. JSON will be chosen by default.
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
}


TEST_P(SchedulerHttpApiTest, DefaultAccept)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = "*/*";

  // Only subscribe needs to 'Accept' json or protobuf.
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  Future<Response> response = process::http::streaming::post(
      master.get()->pid,
      "api/v1/scheduler",
      headers,
      serialize(call, contentType),
      contentType);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
}


TEST_F(SchedulerHttpApiTest, GetRequest)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "api/v1/scheduler",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(MethodNotAllowed({"POST"}).status, response);
}


// This test verifies that the scheduler will receive a `BadRequest` response
// when a teardown call is made without including a stream ID header.
TEST_P(SchedulerHttpApiTest, TeardownWithoutStreamId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  v1::FrameworkID frameworkId;

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
    ASSERT_EQ(Response::PIPE, response->type);
    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    auto deserializer = lambda::bind(
        &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

    Reader<Event> responseDecoder(deserializer, reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the framework ID is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
    EXPECT_NE("", event->get().subscribed().framework_id().value());

    frameworkId = event->get().subscribed().framework_id();
  }

  {
    // Send a TEARDOWN call without a stream ID.
    Call call;
    call.set_type(Call::TEARDOWN);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }
}


// This test verifies that the scheduler will receive a `BadRequest` response
// when a teardown call is made with an incorrect stream ID header.
TEST_P(SchedulerHttpApiTest, TeardownWrongStreamId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;

  v1::FrameworkID frameworkId;
  string streamId;

  // Subscribe once to get a valid stream ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
    ASSERT_EQ(Response::PIPE, response->type);
    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));

    streamId = response->headers.at("Mesos-Stream-Id");

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    auto deserializer = lambda::bind(
        &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

    Reader<Event> responseDecoder(deserializer, reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check that the event type is subscribed and the framework ID is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
    EXPECT_NE("", event->get().subscribed().framework_id().value());

    frameworkId = event->get().subscribed().framework_id();
  }

  // Subscribe again to invalidate the first stream ID and acquire another one.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    // Set the framework ID in the subscribe call.
    call.mutable_framework_id()->CopyFrom(frameworkId);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
    ASSERT_EQ(Response::PIPE, response->type);
    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));

    // Make sure that the new stream ID is different.
    ASSERT_NE(streamId, response->headers.at("Mesos-Stream-Id"));

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    auto deserializer = lambda::bind(
        &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

    Reader<Event> responseDecoder(deserializer, reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());
    EXPECT_NE("", event->get().subscribed().framework_id().value());
  }

  {
    Call call;
    call.set_type(Call::TEARDOWN);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    // Send the first (now incorrect) stream ID with the teardown call.
    headers["Mesos-Stream-Id"] = streamId;

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }
}


// This test verifies that the scheduler will receive a `BadRequest` response
// when it tries to acknowledge a status update with a malformed UUID.
TEST_P(SchedulerHttpApiTest, MalformedUUID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Retrieve the parameter passed as content type to this test.
  const string contentType = GetParam();

  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = contentType;
  v1::FrameworkID frameworkId;
  string streamId;

  // Subscribe once to get a valid stream ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    Future<Response> response = process::http::streaming::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(Response::PIPE, response->type);
    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));

    streamId = response->headers.at("Mesos-Stream-Id");

    Option<Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    auto deserializer = lambda::bind(
        &SchedulerHttpApiTest::deserialize, this, contentType, lambda::_1);

    Reader<Event> responseDecoder(deserializer, reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check that the event type is subscribed and the framework ID is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    frameworkId = event->get().subscribed().framework_id();
    EXPECT_NE("", frameworkId.value());
  }

  // Make an acknowledge call with a malformed UUID. This should result in a
  // `BadResponse`.
  {
    headers["Mesos-Stream-Id"] = streamId;

    Call call;
    call.set_type(Call::ACKNOWLEDGE);

    // Set the framework ID in the subscribe call.
    call.mutable_framework_id()->CopyFrom(frameworkId);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->set_value("task-id");
    acknowledge->mutable_agent_id()->set_value("agent-id");

    // Set a malformed uuid.
    acknowledge->set_uuid("bad-uuid");

    Future<Response> response = process::http::post(
        master.get()->pid,
        "api/v1/scheduler",
        headers,
        serialize(call, contentType),
        contentType);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    AWAIT_EXPECT_RESPONSE_BODY_EQ(
        "Failed to validate scheduler::Call: Not a valid UUID", response);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
