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

#include <mesos/v1/executor/executor.hpp>

#include <mesos/http.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/message.hpp>
#include <process/pid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::recordio::Reader;

using mesos::internal::slave::Slave;

using mesos::v1::executor::Call;
using mesos::v1::executor::Event;

using process::Clock;
using process::Future;
using process::Message;
using process::PID;

using process::http::BadRequest;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Pipe;
using process::http::Response;
using process::http::UnsupportedMediaType;

using recordio::Decoder;

using std::string;
using std::vector;

using testing::Eq;
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
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");
  call.set_type(Call::MESSAGE);

  call.mutable_message()->set_data("hello world");

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

  process::http::Headers headers;
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
  process::http::Headers headers;
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
  process::http::Headers headers;
  headers["Accept"] = stringify(contentType);

  Call call;
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");
  call.set_type(Call::SUBSCRIBE);

  call.mutable_subscribe();

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
  process::http::Headers headers;
  headers["Accept"] = stringify(contentType);

  Call call;
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");
  call.set_type(Call::MESSAGE);

  call.mutable_message()->set_data("hello world");

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
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(MethodNotAllowed({"POST"}).status, response);

  Shutdown();
}


// This test sends in a Accept:*/* header meaning it would
// accept any media type in the response. We expect the
// default "application/json" media type.
TEST_P(ExecutorHttpApiTest, DefaultAccept)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  MockExecutor exec(executorId);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  TaskInfo taskInfo = createTask(offers.get()[0], "", executorId);
  driver.launchTasks(offers.get()[0].id(), {taskInfo});

  // Wait until status update is received on the scheduler
  // before sending an executor subscribe request.
  AWAIT_READY(statusUpdate);

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));
  call.mutable_executor_id()->CopyFrom(evolve(executorId));

  call.set_type(Call::SUBSCRIBE);

  call.mutable_subscribe();

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  process::http::Headers headers;
  headers["Accept"] = "*/*";

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

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  MockExecutor exec(executorId);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  TaskInfo taskInfo = createTask(offers.get()[0], "", executorId);
  driver.launchTasks(offers.get()[0].id(), {taskInfo});

  // Wait until status update is received on the scheduler before sending
  // an executor subscribe request.
  AWAIT_READY(statusUpdate);

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));
  call.mutable_executor_id()->CopyFrom(evolve(executorId));

  call.set_type(Call::SUBSCRIBE);

  call.mutable_subscribe();

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  // No 'Accept' header leads to all media types considered
  // acceptable. JSON will be chosen by default.
  process::http::Headers headers;

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

  process::http::Headers headers;
  headers["Accept"] = "foo";

  // Only subscribe needs to 'Accept' JSON or protobuf.
  Call call;
  call.mutable_framework_id()->set_value("dummy_framework_id");
  call.mutable_executor_id()->set_value("dummy_executor_id");

  call.set_type(Call::SUBSCRIBE);

  call.mutable_subscribe();

  Future<Response> response = process::http::streaming::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(NotAcceptable().status, response);

  Shutdown();
}


TEST_P(ExecutorHttpApiTest, ValidProtobufInvalidCall)
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

  // We send a Call protobuf message with missing
  // required message per type.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("dummy_executor_id");

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> response = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }

  {
    Call call;
    call.set_type(Call::UPDATE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("dummy_executor_id");

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> response = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }

  {
    Call call;
    call.set_type(Call::MESSAGE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("dummy_executor_id");

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> response = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }

  Shutdown();
}


TEST_P(ExecutorHttpApiTest, StatusUpdateCallFailedValidation)
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

  // We send a Call::Update message with inconsistent executor id between
  // Call::executor_id and Call::Update::TaskInfo::executor_id.
  // This should result in failed validation.
  {
    Call call;
    call.set_type(Call::UPDATE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("call_level_executor_id");

    v1::TaskStatus* status = call.mutable_update()->mutable_status();

    status->mutable_executor_id()->set_value("update_level_executor_id");
    status->set_state(mesos::v1::TaskState::TASK_STARTING);
    status->mutable_task_id()->set_value("dummy_task_id");

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> response = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  }

  // We send a Call Update message with a TASK_STAGING
  // status update. This should fail validation.
  {
    Call call;
    call.set_type(Call::UPDATE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("call_level_executor_id");

    v1::TaskStatus* status = call.mutable_update()->mutable_status();

    status->mutable_executor_id()->set_value("call_level_executor_id");
    status->mutable_task_id()->set_value("dummy_task_id");
    status->set_state(mesos::v1::TaskState::TASK_STAGING);

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> responseStatusUpdate = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, responseStatusUpdate);
  }

  // We send a Call Update message with a different source than
  // SOURCE_EXECUTOR in the status update. This should fail validation.
  {
    Call call;
    call.set_type(Call::UPDATE);
    call.mutable_framework_id()->set_value("dummy_framework_id");
    call.mutable_executor_id()->set_value("call_level_executor_id");

    v1::TaskStatus* status = call.mutable_update()->mutable_status();

    status->mutable_executor_id()->set_value("call_level_executor_id");
    status->mutable_task_id()->set_value("dummy_task_id");
    status->set_state(mesos::v1::TaskState::TASK_STARTING);
    status->set_source(mesos::v1::TaskStatus::SOURCE_MASTER);

    process::http::Headers headers;
    headers["Accept"] = APPLICATION_JSON;

    Future<Response> responseStatusUpdate = process::http::post(
        slave.get(),
        "api/v1/executor",
        headers,
        serialize(ContentType::PROTOBUF, call),
        APPLICATION_PROTOBUF);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, responseStatusUpdate);
  }

  Shutdown();
}


// This test verifies if the executor is able to receive a Subscribed
// event in response to a Subscribe call request.
TEST_P(ExecutorHttpApiTest, Subscribe)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  MockExecutor exec(executorId);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers.get().size());

  Future<Message> registerExecutorMessage =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  TaskInfo taskInfo = createTask(offers.get()[0], "", executorId);
  driver.launchTasks(offers.get()[0].id(), {taskInfo});

  // Drop the `RegisterExecutorMessage` and then send a `Subscribe` request
  // from the HTTP based executor.
  AWAIT_READY(registerExecutorMessage);

  Call call;
  call.mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));
  call.mutable_executor_id()->CopyFrom(evolve(executorId));

  call.set_type(Call::SUBSCRIBE);

  call.mutable_subscribe();

  // Retrieve the parameter passed as content type to this test.
  const ContentType contentType = GetParam();

  process::http::Headers headers;
  headers["Accept"] = stringify(contentType);

  Future<Response> response = process::http::streaming::post(
      slave.get(),
      "api/v1/executor",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  EXPECT_SOME_EQ(stringify(contentType),
                 response.get().headers.get("Content-Type"));

  EXPECT_SOME_EQ("chunked",
                 response.get().headers.get("Transfer-Encoding"));

  ASSERT_EQ(Response::PIPE, response.get().type);

  Option<Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer =
    lambda::bind(deserialize<Event>, contentType, lambda::_1);

  Reader<Event> responseDecoder(
      Decoder<Event>(deserializer),
      reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and if the ExecutorID matches.
  ASSERT_EQ(Event::SUBSCRIBED, event.get().get().type());
  ASSERT_EQ(event.get().get().subscribed().executor_info().executor_id(),
            call.executor_id());

  reader.get().close();
  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
