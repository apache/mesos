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

#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/slave/containerizer.hpp>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/limiter.hpp>
#include <process/logging.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include "common/build.hpp"
#include "common/http.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/slave.hpp"
#include "slave/validation.hpp"

#include "version/version.hpp"

using mesos::agent::ProcessIO;

using mesos::authorization::createSubject;

using mesos::internal::recordio::Reader;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerTermination;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Clock;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Logging;
using process::loop;
using process::Owned;
using process::TLDR;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Connection;
using process::http::Forbidden;
using process::http::NotFound;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::UnsupportedMediaType;

using process::http::authentication::Principal;

using process::metrics::internal::MetricsProcess;

using ::recordio::Decoder;

using std::list;
using std::string;
using std::tie;
using std::tuple;
using std::vector;


namespace mesos {

static void json(JSON::ObjectWriter* writer, const TaskInfo& task)
{
  writer->field("id", task.task_id().value());
  writer->field("name", task.name());
  writer->field("slave_id", task.slave_id().value());
  writer->field("resources", Resources(task.resources()));

  // Tasks are not allowed to mix resources allocated to
  // different roles, see MESOS-6636.
  writer->field("role", task.resources().begin()->allocation_info().role());

  if (task.has_command()) {
    writer->field("command", task.command());
  }
  if (task.has_executor()) {
    writer->field("executor_id", task.executor().executor_id().value());
  }
  if (task.has_discovery()) {
    writer->field("discovery", JSON::Protobuf(task.discovery()));
  }
}

static void json(
    JSON::StringWriter* writer,
    const SlaveInfo::Capability& capability)
{
  writer->append(SlaveInfo::Capability::Type_Name(capability.type()));
}

namespace internal {
namespace slave {

// Pull in the process definitions.
using process::http::Response;
using process::http::Request;


// Filtered representation of an Executor. Tasks within this executor
// are filtered based on whether the user is authorized to view them.
struct ExecutorWriter
{
  ExecutorWriter(
      const Owned<ObjectApprover>& taskApprover,
      const Executor* executor,
      const Framework* framework)
    : taskApprover_(taskApprover),
      executor_(executor),
      framework_(framework) {}

  void operator()(JSON::ObjectWriter* writer) const
  {
    writer->field("id", executor_->id.value());
    writer->field("name", executor_->info.name());
    writer->field("source", executor_->info.source());
    writer->field("container", executor_->containerId.value());
    writer->field("directory", executor_->directory);
    writer->field("resources", executor_->resources);

    // Resources may be empty for command executors.
    if (!executor_->info.resources().empty()) {
      // Executors are not allowed to mix resources allocated to
      // different roles, see MESOS-6636.
      writer->field(
          "role",
          executor_->info.resources().begin()->allocation_info().role());
    }

    if (executor_->info.has_labels()) {
      writer->field("labels", executor_->info.labels());
    }

    if (executor_->info.has_type()) {
      writer->field("type", ExecutorInfo::Type_Name(executor_->info.type()));
    }

    writer->field("tasks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Task* task, executor_->launchedTasks) {
        if (!approveViewTask(taskApprover_, *task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });

    writer->field("queued_tasks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (const TaskInfo& task, executor_->queuedTasks) {
        if (!approveViewTaskInfo(taskApprover_, task, framework_->info)) {
          continue;
        }

        writer->element(task);
      }
    });

    writer->field("completed_tasks", [this](JSON::ArrayWriter* writer) {
      foreach (const std::shared_ptr<Task>& task, executor_->completedTasks) {
        if (!approveViewTask(taskApprover_, *task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }

      // NOTE: We add 'terminatedTasks' to 'completed_tasks' for
      // simplicity.
      foreachvalue (Task* task, executor_->terminatedTasks) {
        if (!approveViewTask(taskApprover_, *task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });
  }

  const Owned<ObjectApprover>& taskApprover_;
  const Executor* executor_;
  const Framework* framework_;
};

// Filtered representation of FrameworkInfo.
// Executors and Tasks are filtered based on whether the
// user is authorized to view them.
struct FrameworkWriter
{
  FrameworkWriter(
      const Owned<ObjectApprover>& taskApprover,
      const Owned<ObjectApprover>& executorApprover,
      const Framework* framework)
    : taskApprover_(taskApprover),
      executorApprover_(executorApprover),
      framework_(framework) {}

  void operator()(JSON::ObjectWriter* writer) const
  {
    writer->field("id", framework_->id().value());
    writer->field("name", framework_->info.name());
    writer->field("user", framework_->info.user());
    writer->field("failover_timeout", framework_->info.failover_timeout());
    writer->field("checkpoint", framework_->info.checkpoint());
    writer->field("hostname", framework_->info.hostname());

    if (framework_->info.has_principal()) {
      writer->field("principal", framework_->info.principal());
    }

    // For multi-role frameworks the `role` field will be unset.
    // Note that we could set `roles` here for both cases, which
    // would make tooling simpler (only need to look for `roles`).
    // However, we opted to just mirror the protobuf akin to how
    // generic protobuf -> JSON translation works.
    if (framework_->capabilities.multiRole) {
      writer->field("roles", framework_->info.roles());
    } else {
      writer->field("role", framework_->info.role());
    }

    writer->field("executors", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Executor* executor, framework_->executors) {
        if (!approveViewExecutorInfo(
                executorApprover_, executor->info, framework_->info)) {
          continue;
        }

        ExecutorWriter executorWriter(
            taskApprover_,
            executor,
            framework_);

        writer->element(executorWriter);
      }
    });

    writer->field(
        "completed_executors", [this](JSON::ArrayWriter* writer) {
          foreach (
              const Owned<Executor>& executor, framework_->completedExecutors) {
            if (!approveViewExecutorInfo(
                executorApprover_, executor->info, framework_->info)) {
              continue;
            }

            ExecutorWriter executorWriter(
                taskApprover_,
                executor.get(),
                framework_);

            writer->element(executorWriter);
          }
        });
  }

  const Owned<ObjectApprover>& taskApprover_;
  const Owned<ObjectApprover>& executorApprover_;
  const Framework* framework_;
};


string Slave::Http::API_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for API calls against the agent."),
    DESCRIPTION(
        "Returns 200 OK if the call is successful"),
    AUTHENTICATION(true));
}


Future<Response> Slave::Http::api(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(anand): Add metrics for rejected requests.

  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  Option<string> contentType_ = request.headers.get("Content-Type");
  if (contentType_.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  ContentType contentType;
  if (contentType_.get() == APPLICATION_JSON) {
    contentType = ContentType::JSON;
  } else if (contentType_.get() == APPLICATION_PROTOBUF) {
    contentType = ContentType::PROTOBUF;
  } else if (contentType_.get() == APPLICATION_RECORDIO) {
    contentType = ContentType::RECORDIO;
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF +
        + " or " + APPLICATION_RECORDIO);
  }

  Option<ContentType> messageContentType;
  Option<string> messageContentType_ =
    request.headers.get(MESSAGE_CONTENT_TYPE);

  if (streamingMediaType(contentType)) {
    if (messageContentType_.isNone()) {
      return BadRequest(
          "Expecting '" + stringify(MESSAGE_CONTENT_TYPE) + "' to be" +
          " set for streaming requests");
    }

    if (messageContentType_.get() == APPLICATION_JSON) {
      messageContentType = Option<ContentType>(ContentType::JSON);
    } else if (messageContentType_.get() == APPLICATION_PROTOBUF) {
      messageContentType = Option<ContentType>(ContentType::PROTOBUF);
    } else {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' of " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Content-Type"
    // header for a non-streaming request.
    if (messageContentType_.isSome()) {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' to be not"
          " set for non-streaming requests");
    }
  }

  // This lambda deserializes a string into a valid `Call`
  // based on the content type.
  auto deserializer = [](const string& body, ContentType contentType)
      -> Try<mesos::agent::Call> {
    Try<v1::agent::Call> v1Call =
      deserialize<v1::agent::Call>(contentType, body);

    if (v1Call.isError()) {
      return Error(v1Call.error());
    }

    mesos::agent::Call call = devolve(v1Call.get());

    Option<Error> error = validation::agent::call::validate(call);
    if (error.isSome()) {
      return Error("Failed to validate agent::Call: " + error.get().message);
    }

    return call;
  };

  // For backwards compatibility, if a client does not specify an 'Accept'
  // header, 'Content-Type' of the response is set to 'application/json'
  // for streaming responses.
  //
  // TODO(anand): In v2 API, the default 'Content-Type' for streaming responses
  // should be 'application/recordio'.
  ContentType acceptType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    acceptType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    acceptType = ContentType::PROTOBUF;
  } else if (request.acceptsMediaType(APPLICATION_RECORDIO)) {
    acceptType = ContentType::RECORDIO;
  } else {
    return NotAcceptable(
        string("Expecting 'Accept' to allow ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF + " or " +
        APPLICATION_RECORDIO);
  }

  Option<ContentType> messageAcceptType;
  if (streamingMediaType(acceptType)) {
    if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_JSON)) {
      messageAcceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_PROTOBUF)) {
      messageAcceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT + "' to allow " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Accept"
    // header for a non-streaming response.
    if (request.headers.contains(MESSAGE_ACCEPT)) {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT +
          "' to be not set for non-streaming responses");
    }
  }

  CHECK_EQ(Request::PIPE, request.type);
  CHECK_SOME(request.reader);

  RequestMediaTypes mediaTypes {
      contentType, acceptType, messageContentType, messageAcceptType};

  if (streamingMediaType(contentType)) {
    CHECK_SOME(mediaTypes.messageContent);

    Owned<Reader<mesos::agent::Call>> reader(new Reader<mesos::agent::Call>(
        Decoder<mesos::agent::Call>(lambda::bind(
            deserializer, lambda::_1, mediaTypes.messageContent.get())),
        request.reader.get()));

    return reader->read()
      .then(defer(
          slave->self(),
          [=](const Result<mesos::agent::Call>& call) -> Future<Response> {
            if (call.isNone()) {
              return BadRequest("Received EOF while reading request body");
            }

            if (call.isError()) {
              return BadRequest(call.error());
            }

            return _api(call.get(), std::move(reader), mediaTypes, principal);
          }));
  } else {
    Pipe::Reader reader = request.reader.get();  // Remove const.

    return reader.readAll()
      .then(defer(
          slave->self(),
          [=](const string& body) -> Future<Response> {
            Try<mesos::agent::Call> call = deserializer(body, contentType);
            if (call.isError()) {
              return BadRequest(call.error());
            }
            return _api(call.get(), None(), mediaTypes, principal);
          }));
  }
}


Future<Response> Slave::Http::_api(
    const mesos::agent::Call& call,
    Option<Owned<Reader<mesos::agent::Call>>>&& reader,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  // Validate that a client has not _accidentally_ sent us a
  // streaming request for a call type that does not support it.
  if (streamingMediaType(mediaTypes.content) &&
      call.type() != mesos::agent::Call::ATTACH_CONTAINER_INPUT) {
    return UnsupportedMediaType(
        "Streaming 'Content-Type' " + stringify(mediaTypes.content) + " is not "
        "supported for " + stringify(call.type()) + " call");
  } else if (!streamingMediaType(mediaTypes.content) &&
             call.type() == mesos::agent::Call::ATTACH_CONTAINER_INPUT) {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' to be ") + APPLICATION_RECORDIO +
        " for " + stringify(call.type()) + " call");
  }

  LOG(INFO) << "Processing call " << call.type();

  switch (call.type()) {
    case mesos::agent::Call::UNKNOWN:
      return NotImplemented();

    case mesos::agent::Call::GET_HEALTH:
      return getHealth(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_FLAGS:
      return getFlags(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_VERSION:
      return getVersion(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_METRICS:
      return getMetrics(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_LOGGING_LEVEL:
      return getLoggingLevel(call, mediaTypes.accept, principal);

    case mesos::agent::Call::SET_LOGGING_LEVEL:
      return setLoggingLevel(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LIST_FILES:
      return listFiles(call, mediaTypes.accept, principal);

    case mesos::agent::Call::READ_FILE:
      return readFile(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_STATE:
      return getState(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_CONTAINERS:
      return getContainers(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_FRAMEWORKS:
      return getFrameworks(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_EXECUTORS:
      return getExecutors(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_TASKS:
      return getTasks(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_AGENT:
      return getAgent(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER:
      return launchNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::WAIT_NESTED_CONTAINER:
      return waitNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::KILL_NESTED_CONTAINER:
      return killNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::REMOVE_NESTED_CONTAINER:
      return removeNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION:
      return launchNestedContainerSession(call, mediaTypes, principal);

    case mesos::agent::Call::ATTACH_CONTAINER_INPUT:
      CHECK_SOME(reader);
      return attachContainerInput(
          call, std::move(reader).get(), mediaTypes, principal);

    case mesos::agent::Call::ATTACH_CONTAINER_OUTPUT:
      return attachContainerOutput(call, mediaTypes, principal);
  }

  UNREACHABLE();
}


string Slave::Http::EXECUTOR_HELP() {
  return HELP(
    TLDR(
        "Endpoint for the Executor HTTP API."),
    DESCRIPTION(
        "This endpoint is used by the executors to interact with the",
        "agent via Call/Event messages.",
        "",
        "Returns 200 OK iff the initial SUBSCRIBE Call is successful.",
        "This will result in a streaming response via chunked",
        "transfer encoding. The executors can process the response",
        "incrementally.",
        "",
        "Returns 202 Accepted for all other Call messages iff the",
        "request is accepted."),
    AUTHENTICATION(false));
}


// TODO(greggomann): Remove this function when implicit executor authorization
// is moved into the authorizer. See MESOS-7399.
Option<Error> verifyExecutorClaims(
    const Principal& principal,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId) {
  if (!(principal.claims.contains("fid") &&
        principal.claims.at("fid") == frameworkId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain an 'fid' claim with the framework ID " +
        stringify(frameworkId) + ", which is set in the call");
  }

  if (!(principal.claims.contains("eid") &&
        principal.claims.at("eid") == executorId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain an 'eid' claim with the executor ID " +
        stringify(executorId) + ", which is set in the call");
  }

  if (!(principal.claims.contains("cid") &&
        principal.claims.at("cid") == containerId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain a 'cid' claim with the correct active ContainerID");
  }

  return None();
}


Future<Response> Slave::Http::executor(
    const Request& request,
    const Option<Principal>& principal) const
{
  if (!slave->recoveryInfo.reconnect) {
    CHECK(slave->state == RECOVERING);
    return ServiceUnavailable("Agent has not finished recovery");
  }

  // TODO(anand): Add metrics for rejected requests.

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::executor::Call v1Call;

  Option<string> contentType = request.headers.get("Content-Type");
  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!v1Call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);
    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::executor::Call> parse =
      ::protobuf::parse<v1::executor::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  const executor::Call call = devolve(v1Call);

  Option<Error> error = validation::executor::call::validate(call);

  if (error.isSome()) {
    return BadRequest("Failed to validate Executor::Call: " +
                      error.get().message);
  }

  ContentType acceptType;

  if (call.type() == executor::Call::SUBSCRIBE) {
    // We default to JSON since an empty 'Accept' header
    // results in all media types considered acceptable.
    if (request.acceptsMediaType(APPLICATION_JSON)) {
      acceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      acceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }
  } else {
    if (slave->state == Slave::RECOVERING) {
      return ServiceUnavailable("Agent has not finished recovery");
    }
  }

  // We consolidate the framework/executor lookup logic here because
  // it is common for all the call handlers.
  Framework* framework = slave->getFramework(call.framework_id());
  if (framework == nullptr) {
    return BadRequest("Framework cannot be found");
  }

  Executor* executor = framework->getExecutor(call.executor_id());
  if (executor == nullptr) {
    return BadRequest("Executor cannot be found");
  }

  // TODO(greggomann): Move this implicit executor authorization
  // into the authorizer. See MESOS-7399.
  if (principal.isSome()) {
    error = verifyExecutorClaims(
        principal.get(),
        call.framework_id(),
        call.executor_id(),
        executor->containerId);

    if (error.isSome()) {
      return Forbidden(error->message);
    }
  }

  if (executor->state == Executor::REGISTERING &&
      call.type() != executor::Call::SUBSCRIBE) {
    return Forbidden("Executor is not subscribed");
  }

  switch (call.type()) {
    case executor::Call::SUBSCRIBE: {
      Pipe pipe;
      OK ok;
      ok.headers["Content-Type"] = stringify(acceptType);

      ok.type = Response::PIPE;
      ok.reader = pipe.reader();

      HttpConnection http {pipe.writer(), acceptType};
      slave->subscribe(http, call.subscribe(), framework, executor);

      return ok;
    }

    case executor::Call::UPDATE: {
      slave->statusUpdate(protobuf::createStatusUpdate(
          call.framework_id(),
          call.update().status(),
          slave->info.id()),
          None());

      return Accepted();
    }

    case executor::Call::MESSAGE: {
      slave->executorMessage(
          slave->info.id(),
          framework->id(),
          executor->id,
          call.message().data());

      return Accepted();
    }

    case executor::Call::UNKNOWN: {
      LOG(WARNING) << "Received 'UNKNOWN' call";
      return NotImplemented();
    }
  }

  UNREACHABLE();
}


string Slave::Http::FLAGS_HELP()
{
  return HELP(
    TLDR("Exposes the agent's flag configuration."),
    None(),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The request principal should be authorized to view all flags.",
        "See the authorization documentation for details."));
}


Future<Response> Slave::Http::flags(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  if (slave->authorizer.isNone()) {
    return OK(_flags(), request.url.query.get("jsonp"));
  }

  authorization::Request authRequest;
  authRequest.set_action(authorization::VIEW_FLAGS);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    authRequest.mutable_subject()->CopyFrom(subject.get());
  }

  return slave->authorizer.get()->authorized(authRequest)
      .then(defer(
          slave->self(),
          [this, request](bool authorized) -> Future<Response> {
            if (authorized) {
              return OK(_flags(), request.url.query.get("jsonp"));
            } else {
              return Forbidden();
            }
          }));
}


JSON::Object Slave::Http::_flags() const
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachvalue (const flags::Flag& flag, slave->flags) {
      Option<string> value = flag.stringify(slave->flags);
      if (value.isSome()) {
        flags.values[flag.effective_name().value] = value.get();
      }
    }
    object.values["flags"] = std::move(flags);
  }

  return object;
}


Future<Response> Slave::Http::getFlags(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_FLAGS, call.type());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FLAGS);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
      [this, acceptType](
          const Owned<ObjectApprover>& approver) -> Future<Response> {
        Try<bool> approved = approver->approved(ObjectApprover::Object());

        if (approved.isError()) {
          return InternalServerError(approved.error());
        } else if (!approved.get()) {
          return Forbidden();
        }

        return OK(
            serialize(
                acceptType, evolve<v1::agent::Response::GET_FLAGS>(_flags())),
            stringify(acceptType));
      }));
}


string Slave::Http::HEALTH_HELP()
{
  return HELP(
    TLDR(
        "Health check of the Agent."),
    DESCRIPTION(
        "Returns 200 OK iff the Agent is healthy.",
        "Delayed responses are also indicative of poor health."),
    AUTHENTICATION(false));
}


Future<Response> Slave::Http::health(const Request& request) const
{
  return OK();
}


Future<Response> Slave::Http::getHealth(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_HEALTH, call.type());

  mesos::agent::Response response;
  response.set_type(mesos::agent::Response::GET_HEALTH);
  response.mutable_get_health()->set_healthy(true);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Slave::Http::getVersion(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_VERSION, call.type());

  return OK(serialize(acceptType,
                      evolve<v1::agent::Response::GET_VERSION>(version())),
            stringify(acceptType));
}


Future<Response> Slave::Http::getMetrics(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_METRICS, call.type());
  CHECK(call.has_get_metrics());

  Option<Duration> timeout;
  if (call.get_metrics().has_timeout()) {
    timeout = Nanoseconds(call.get_metrics().timeout().nanoseconds());
  }

  return process::metrics::snapshot(timeout)
      .then([acceptType](const hashmap<string, double>& metrics) -> Response {
          mesos::agent::Response response;
        response.set_type(mesos::agent::Response::GET_METRICS);
        mesos::agent::Response::GetMetrics* _getMetrics =
          response.mutable_get_metrics();

        foreachpair (const string& key, double value, metrics) {
          Metric* metric = _getMetrics->add_metrics();
          metric->set_name(key);
          metric->set_value(value);
        }

        return OK(serialize(acceptType, evolve(response)),
                  stringify(acceptType));
      });
}


Future<Response> Slave::Http::getLoggingLevel(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_LOGGING_LEVEL, call.type());

  mesos::agent::Response response;
  response.set_type(mesos::agent::Response::GET_LOGGING_LEVEL);
  response.mutable_get_logging_level()->set_level(FLAGS_v);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Slave::Http::setLoggingLevel(
    const mesos::agent::Call& call,
    ContentType /*contentType*/,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::SET_LOGGING_LEVEL, call.type());
  CHECK(call.has_set_logging_level());

  uint32_t level = call.set_logging_level().level();
  Duration duration =
    Nanoseconds(call.set_logging_level().duration().nanoseconds());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::SET_LOG_LEVEL);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(
      [level, duration](
          const Owned<ObjectApprover>& approver) -> Future<Response> {
        Try<bool> approved = approver->approved((ObjectApprover::Object()));

        if (approved.isError()) {
          return InternalServerError(approved.error());
        } else if (!approved.get()) {
          return Forbidden();
        }

        return dispatch(
            process::logging(), &Logging::set_level, level, duration)
          .then([]() -> Response {
            return OK();
          });
      });
}


Future<Response> Slave::Http::listFiles(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LIST_FILES, call.type());

  const string& path = call.list_files().path();

  return slave->files->browse(path, principal)
    .then([acceptType](const Try<list<FileInfo>, FilesError>& result)
      -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::LIST_FILES);

      mesos::agent::Response::ListFiles* listFiles =
        response.mutable_list_files();

      foreach (const FileInfo& fileInfo, result.get()) {
        listFiles->add_file_infos()->CopyFrom(fileInfo);
      }

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    });
}


string Slave::Http::STATE_HELP() {
  return HELP(
    TLDR(
        "Information about state of the Agent."),
    DESCRIPTION(
        "This endpoint shows information about the frameworks, executors",
        "and the agent's master as a JSON object.",
        "The information shown might be filtered based on the user",
        "accessing the endpoint.",
        "",
        "Example (**Note**: this is not exhaustive):",
        "",
        "```",
        "{",
        "    \"version\" : \"0.28.0\",",
        "    \"git_sha\" : \"9d5889b5a265849886a533965f4aefefd1fbd103\",",
        "    \"git_branch\" : \"refs/heads/master\",",
        "    \"git_tag\" : \"0.28.0\",",
        "    \"build_date\" : \"2016-02-15 10:00:28\"",
        "    \"build_time\" : 1455559228,",
        "    \"build_user\" : \"mesos-user\",",
        "    \"start_time\" : 1455647422.88396,",
        "    \"id\" : \"e2c38084-f6ea-496f-bce3-b6e07cea5e01-S0\",",
        "    \"pid\" : \"slave(1)@127.0.1.1:5051\",",
        "    \"hostname\" : \"localhost\",",
        "    \"resources\" : {",
        "         \"ports\" : \"[31000-32000]\",",
        "         \"mem\" : 127816,",
        "         \"disk\" : 804211,",
        "         \"cpus\" : 32",
        "    },",
        "    \"attributes\" : {},",
        "    \"master_hostname\" : \"localhost\",",
        "    \"log_dir\" : \"/var/log\",",
        "    \"external_log_file\" : \"mesos.log\",",
        "    \"frameworks\" : [],",
        "    \"completed_frameworks\" : [],",
        "    \"flags\" : {",
        "         \"gc_disk_headroom\" : \"0.1\",",
        "         \"isolation\" : \"posix/cpu,posix/mem\",",
        "         \"containerizers\" : \"mesos\",",
        "         \"docker_socket\" : \"/var/run/docker.sock\",",
        "         \"gc_delay\" : \"1weeks\",",
        "         \"docker_remove_delay\" : \"6hrs\",",
        "         \"port\" : \"5051\",",
        "         \"systemd_runtime_directory\" : \"/run/systemd/system\",",
        "         \"initialize_driver_logging\" : \"true\",",
        "         \"cgroups_root\" : \"mesos\",",
        "         \"fetcher_cache_size\" : \"2GB\",",
        "         \"cgroups_hierarchy\" : \"/sys/fs/cgroup\",",
        "         \"qos_correction_interval_min\" : \"0ns\",",
        "         \"cgroups_cpu_enable_pids_and_tids_count\" : \"false\",",
        "         \"sandbox_directory\" : \"/mnt/mesos/sandbox\",",
        "         \"docker\" : \"docker\",",
        "         \"help\" : \"false\",",
        "         \"docker_stop_timeout\" : \"0ns\",",
        "         \"master\" : \"127.0.0.1:5050\",",
        "         \"logbufsecs\" : \"0\",",
        "         \"docker_registry\" : \"https://registry-1.docker.io\",",
        "         \"frameworks_home\" : \"\",",
        "         \"cgroups_enable_cfs\" : \"false\",",
        "         \"perf_interval\" : \"1mins\",",
        "         \"docker_kill_orphans\" : \"true\",",
        "         \"switch_user\" : \"true\",",
        "         \"logging_level\" : \"INFO\",",
        "         \"hadoop_home\" : \"\",",
        "         \"strict\" : \"true\",",
        "         \"executor_registration_timeout\" : \"1mins\",",
        "         \"recovery_timeout\" : \"15mins\",",
        "         \"revocable_cpu_low_priority\" : \"true\",",
        "         \"docker_store_dir\" : \"/tmp/mesos/store/docker\",",
        "         \"image_provisioner_backend\" : \"copy\",",
        "         \"authenticatee\" : \"crammd5\",",
        "         \"quiet\" : \"false\",",
        "         \"executor_shutdown_grace_period\" : \"5secs\",",
        "         \"fetcher_cache_dir\" : \"/tmp/mesos/fetch\",",
        "         \"default_role\" : \"*\",",
        "         \"work_dir\" : \"/tmp/mesos\",",
        "         \"launcher_dir\" : \"/path/to/mesos/build/src\",",
        "         \"registration_backoff_factor\" : \"1secs\",",
        "         \"oversubscribed_resources_interval\" : \"15secs\",",
        "         \"enforce_container_disk_quota\" : \"false\",",
        "         \"container_disk_watch_interval\" : \"15secs\",",
        "         \"disk_watch_interval\" : \"1mins\",",
        "         \"cgroups_limit_swap\" : \"false\",",
        "         \"hostname_lookup\" : \"true\",",
        "         \"perf_duration\" : \"10secs\",",
        "         \"appc_store_dir\" : \"/tmp/mesos/store/appc\",",
        "         \"recover\" : \"reconnect\",",
        "         \"version\" : \"false\"",
        "    },",
        "}",
        "```"),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "For example a user might only see the subset of frameworks,",
        "tasks, and executors they are allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Slave::Http::state(
    const Request& request,
    const Option<Principal>& principal) const
{
  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  // Retrieve `ObjectApprover`s for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  Future<Owned<ObjectApprover>> flagsApprover;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    tasksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_TASK);

    executorsApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);

    flagsApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FLAGS);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    tasksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    flagsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(
      frameworksApprover,
      tasksApprover,
      executorsApprover,
      flagsApprover)
    .then(defer(
        slave->self(),
        [this, request](const tuple<Owned<ObjectApprover>,
                                    Owned<ObjectApprover>,
                                    Owned<ObjectApprover>,
                                    Owned<ObjectApprover>>& approvers)
          -> Response {
      // This lambda is consumed before the outer lambda
      // returns, hence capture by reference is fine here.
      auto state = [this, &approvers](JSON::ObjectWriter* writer) {
        // Get approver from tuple.
        Owned<ObjectApprover> frameworksApprover;
        Owned<ObjectApprover> tasksApprover;
        Owned<ObjectApprover> executorsApprover;
        Owned<ObjectApprover> flagsApprover;
        tie(frameworksApprover,
            tasksApprover,
            executorsApprover,
            flagsApprover) = approvers;

        writer->field("version", MESOS_VERSION);

        if (build::GIT_SHA.isSome()) {
          writer->field("git_sha", build::GIT_SHA.get());
        }

        if (build::GIT_BRANCH.isSome()) {
          writer->field("git_branch", build::GIT_BRANCH.get());
        }

        if (build::GIT_TAG.isSome()) {
          writer->field("git_tag", build::GIT_TAG.get());
        }

        writer->field("build_date", build::DATE);
        writer->field("build_time", build::TIME);
        writer->field("build_user", build::USER);
        writer->field("start_time", slave->startTime.secs());

        writer->field("id", slave->info.id().value());
        writer->field("pid", string(slave->self()));
        writer->field("hostname", slave->info.hostname());
        writer->field("capabilities", AGENT_CAPABILITIES());

        const Resources& totalResources = slave->totalResources;

        writer->field("resources", totalResources);
        writer->field("reserved_resources", totalResources.reservations());
        writer->field("unreserved_resources", totalResources.unreserved());

        writer->field(
            "reserved_resources_full",
            [&totalResources](JSON::ObjectWriter* writer) {
              foreachpair (const string& role,
                           const Resources& resources,
                           totalResources.reservations()) {
                writer->field(role, [&resources](JSON::ArrayWriter* writer) {
                  foreach (const Resource& resource, resources) {
                    writer->element(JSON::Protobuf(resource));
                  }
                });
              }
            });

        writer->field("attributes", Attributes(slave->info.attributes()));

        if (slave->master.isSome()) {
          Try<string> hostname =
            net::getHostname(slave->master.get().address.ip);

          if (hostname.isSome()) {
            writer->field("master_hostname", hostname.get());
          }
        }

        if (approveViewFlags(flagsApprover)) {
          if (slave->flags.log_dir.isSome()) {
            writer->field("log_dir", slave->flags.log_dir.get());
          }

          if (slave->flags.external_log_file.isSome()) {
            writer->field(
                "external_log_file", slave->flags.external_log_file.get());
          }

          writer->field("flags", [this](JSON::ObjectWriter* writer) {
            foreachvalue (const flags::Flag& flag, slave->flags) {
              Option<string> value = flag.stringify(slave->flags);
              if (value.isSome()) {
                writer->field(flag.effective_name().value, value.get());
              }
            }
          });
        }

        // Model all of the frameworks.
        writer->field(
            "frameworks",
            [this, &frameworksApprover, &executorsApprover, &tasksApprover](
                JSON::ArrayWriter* writer) {
          foreachvalue (Framework* framework, slave->frameworks) {
            // Skip unauthorized frameworks.
            if (!approveViewFrameworkInfo(
                    frameworksApprover, framework->info)) {
              continue;
            }

            FrameworkWriter frameworkWriter(
                tasksApprover,
                executorsApprover,
                framework);

            writer->element(frameworkWriter);
          }
        });

        // Model all of the completed frameworks.
        writer->field(
            "completed_frameworks",
            [this, &frameworksApprover, &executorsApprover, &tasksApprover](
                JSON::ArrayWriter* writer) {
          foreachvalue (const Owned<Framework>& framework,
                        slave->completedFrameworks) {
            // Skip unauthorized frameworks.
            if (!approveViewFrameworkInfo(
                    frameworksApprover, framework->info)) {
              continue;
            }

            FrameworkWriter frameworkWriter(
                tasksApprover,
                executorsApprover,
                framework.get());

            writer->element(frameworkWriter);
          }
        });
      };

      return OK(jsonify(state), request.url.query.get("jsonp"));
    }));
}


Future<Response> Slave::Http::getFrameworks(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_FRAMEWORKS, call.type());

  // Retrieve `ObjectApprover`s for authorizing frameworks.
  Future<Owned<ObjectApprover>> frameworksApprover;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return frameworksApprover
    .then(defer(slave->self(),
        [this, acceptType](const Owned<ObjectApprover>& frameworksApprover)
          -> Future<Response> {
          mesos::agent::Response response;
      response.set_type(mesos::agent::Response::GET_FRAMEWORKS);
      response.mutable_get_frameworks()->CopyFrom(
          _getFrameworks(frameworksApprover));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    }));
}


mesos::agent::Response::GetFrameworks Slave::Http::_getFrameworks(
    const Owned<ObjectApprover>& frameworksApprover) const
{
  mesos::agent::Response::GetFrameworks getFrameworks;
  foreachvalue (const Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    getFrameworks.add_frameworks()->mutable_framework_info()
      ->CopyFrom(framework->info);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    getFrameworks.add_completed_frameworks()->mutable_framework_info()
      ->CopyFrom(framework->info);
  }

  return getFrameworks;
}


Future<Response> Slave::Http::getExecutors(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_EXECUTORS, call.type());

  // Retrieve `ObjectApprover`s for authorizing frameworks and executors.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    executorsApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(frameworksApprover, executorsApprover)
    .then(defer(slave->self(),
        [this, acceptType](const tuple<Owned<ObjectApprover>,
                                        Owned<ObjectApprover>>& approvers)
          -> Future<Response> {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, executorsApprover) = approvers;

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::GET_EXECUTORS);

      response.mutable_get_executors()->CopyFrom(
          _getExecutors(frameworksApprover, executorsApprover));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    }));
}


mesos::agent::Response::GetExecutors Slave::Http::_getExecutors(
    const Owned<ObjectApprover>& frameworksApprover,
    const Owned<ObjectApprover>& executorsApprover) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  mesos::agent::Response::GetExecutors getExecutors;

  foreach (const Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Skip unauthorized executors.
      if (!approveViewExecutorInfo(executorsApprover,
                                   executor->info,
                                   framework->info)) {
        continue;
      }

      getExecutors.add_executors()->mutable_executor_info()->CopyFrom(
          executor->info);
    }

    foreach (const Owned<Executor>& executor, framework->completedExecutors) {
      // Skip unauthorized executors.
      if (!approveViewExecutorInfo(executorsApprover,
                                   executor->info,
                                   framework->info)) {
        continue;
      }

      getExecutors.add_completed_executors()->mutable_executor_info()->CopyFrom(
          executor->info);
    }
  }

  return getExecutors;
}


Future<Response> Slave::Http::getTasks(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_TASKS, call.type());

  // Retrieve Approvers for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    tasksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_TASK);

    executorsApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    tasksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(frameworksApprover, tasksApprover, executorsApprover)
    .then(defer(slave->self(),
      [this, acceptType](const tuple<Owned<ObjectApprover>,
                                      Owned<ObjectApprover>,
                                      Owned<ObjectApprover>>& approvers)
        -> Future<Response> {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> tasksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, tasksApprover, executorsApprover) = approvers;

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::GET_TASKS);

      response.mutable_get_tasks()->CopyFrom(
          _getTasks(frameworksApprover,
                    tasksApprover,
                    executorsApprover));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
  }));
}


mesos::agent::Response::GetTasks Slave::Http::_getTasks(
    const Owned<ObjectApprover>& frameworksApprover,
    const Owned<ObjectApprover>& tasksApprover,
    const Owned<ObjectApprover>& executorsApprover) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  // Construct executor list with both active and completed executors.
  hashmap<const Executor*, const Framework*> executors;
  foreach (const Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Skip unauthorized executors.
      if (!approveViewExecutorInfo(executorsApprover,
                                   executor->info,
                                   framework->info)) {
        continue;
      }

      executors.put(executor, framework);
    }

    foreach (const Owned<Executor>& executor, framework->completedExecutors) {
      // Skip unauthorized executors.
      if (!approveViewExecutorInfo(executorsApprover,
                                   executor->info,
                                   framework->info)) {
        continue;
      }

      executors.put(executor.get(), framework);
    }
  }

  mesos::agent::Response::GetTasks getTasks;

  foreach (const Framework* framework, frameworks) {
    // Pending tasks.
    typedef hashmap<TaskID, TaskInfo> TaskMap;
    foreachvalue (const TaskMap& taskInfos, framework->pending) {
      foreachvalue (const TaskInfo& taskInfo, taskInfos) {
        // Skip unauthorized tasks.
        if (!approveViewTaskInfo(tasksApprover, taskInfo, framework->info)) {
          continue;
        }

        const Task& task =
          protobuf::createTask(taskInfo, TASK_STAGING, framework->id());

        getTasks.add_pending_tasks()->CopyFrom(task);
      }
    }
  }

  foreachpair (const Executor* executor,
               const Framework* framework,
               executors) {
    // Queued tasks.
    foreachvalue (const TaskInfo& taskInfo, executor->queuedTasks) {
      // Skip unauthorized tasks.
      if (!approveViewTaskInfo(tasksApprover, taskInfo, framework->info)) {
        continue;
      }

      const Task& task =
        protobuf::createTask(taskInfo, TASK_STAGING, framework->id());

      getTasks.add_queued_tasks()->CopyFrom(task);
    }

    // Launched tasks.
    foreachvalue (Task* task, executor->launchedTasks) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approveViewTask(tasksApprover, *task, framework->info)) {
        continue;
      }

      getTasks.add_launched_tasks()->CopyFrom(*task);
    }

    // Terminated tasks.
    foreachvalue (Task* task, executor->terminatedTasks) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approveViewTask(tasksApprover, *task, framework->info)) {
        continue;
      }

      getTasks.add_terminated_tasks()->CopyFrom(*task);
    }

    // Completed tasks.
    foreach (const std::shared_ptr<Task>& task, executor->completedTasks) {
      // Skip unauthorized tasks.
      if (!approveViewTask(tasksApprover, *task.get(), framework->info)) {
        continue;
      }

      getTasks.add_completed_tasks()->CopyFrom(*task);
    }
  }

  return getTasks;
}


Future<Response> Slave::Http::getAgent(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_AGENT, call.type());

  agent::Response response;
  response.set_type(mesos::agent::Response::GET_AGENT);

  response.mutable_get_agent()->mutable_slave_info()->CopyFrom(slave->info);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Slave::Http::getState(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_STATE, call.type());

  // Retrieve Approvers for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    tasksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_TASK);

    executorsApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    tasksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(frameworksApprover, tasksApprover, executorsApprover)
    .then(defer(slave->self(),
      [=](const tuple<Owned<ObjectApprover>,
                      Owned<ObjectApprover>,
                      Owned<ObjectApprover>>& approvers)
        -> Future<Response> {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> tasksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, tasksApprover, executorsApprover) = approvers;

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::GET_STATE);
      response.mutable_get_state()->CopyFrom(
          _getState(frameworksApprover,
                    tasksApprover,
                    executorsApprover));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    }));
}


mesos::agent::Response::GetState Slave::Http::_getState(
    const Owned<ObjectApprover>& frameworksApprover,
    const Owned<ObjectApprover>& tasksApprover,
    const Owned<ObjectApprover>& executorsApprover) const
{
  mesos::agent::Response::GetState getState;

  getState.mutable_get_tasks()->CopyFrom(
      _getTasks(frameworksApprover, tasksApprover, executorsApprover));

  getState.mutable_get_executors()->CopyFrom(
      _getExecutors(frameworksApprover, executorsApprover));

  getState.mutable_get_frameworks()->CopyFrom(
      _getFrameworks(frameworksApprover));

  return getState;
}


string Slave::Http::STATISTICS_HELP()
{
  return HELP(
      TLDR(
          "Retrieve resource monitoring information."),
      DESCRIPTION(
          "Returns the current resource consumption data for containers",
          "running under this agent.",
          "",
          "Example:",
          "",
          "```",
          "[{",
          "    \"executor_id\":\"executor\",",
          "    \"executor_name\":\"name\",",
          "    \"framework_id\":\"framework\",",
          "    \"source\":\"source\",",
          "    \"statistics\":",
          "    {",
          "        \"cpus_limit\":8.25,",
          "        \"cpus_nr_periods\":769021,",
          "        \"cpus_nr_throttled\":1046,",
          "        \"cpus_system_time_secs\":34501.45,",
          "        \"cpus_throttled_time_secs\":352.597023453,",
          "        \"cpus_user_time_secs\":96348.84,",
          "        \"mem_anon_bytes\":4845449216,",
          "        \"mem_file_bytes\":260165632,",
          "        \"mem_limit_bytes\":7650410496,",
          "        \"mem_mapped_file_bytes\":7159808,",
          "        \"mem_rss_bytes\":5105614848,",
          "        \"timestamp\":1388534400.0",
          "    }",
          "}]",
          "```"),
      AUTHENTICATION(true),
      AUTHORIZATION(
          "The request principal should be authorized to query this endpoint.",
          "See the authorization documentation for details."));
}


Future<Response> Slave::Http::statistics(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Try<string> endpoint = extractEndpoint(request.url);
  if (endpoint.isError()) {
    return Failure("Failed to extract endpoint: " + endpoint.error());
  }

  return authorizeEndpoint(
      endpoint.get(),
      request.method,
      slave->authorizer,
      principal)
    .then(defer(
        slave->self(),
        [this, request](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return statisticsLimiter->acquire()
            .then(defer(slave->self(), &Slave::usage))
            .then(defer(slave->self(),
                  [this, request](const ResourceUsage& usage) {
              return _statistics(usage, request);
            }));
        }));
}


Response Slave::Http::_statistics(
    const ResourceUsage& usage,
    const Request& request) const
{
  JSON::Array result;

  foreach (const ResourceUsage::Executor& executor, usage.executors()) {
    if (executor.has_statistics()) {
      const ExecutorInfo& info = executor.executor_info();

      JSON::Object entry;
      entry.values["framework_id"] = info.framework_id().value();
      entry.values["executor_id"] = info.executor_id().value();
      entry.values["executor_name"] = info.name();
      entry.values["source"] = info.source();
      entry.values["statistics"] = JSON::protobuf(executor.statistics());

      result.values.push_back(entry);
    }
  }

  return OK(result, request.url.query.get("jsonp"));
}


string Slave::Http::CONTAINERS_HELP()
{
  return HELP(
      TLDR(
          "Retrieve container status and usage information."),
      DESCRIPTION(
          "Returns the current resource consumption data and status for",
          "containers running under this slave.",
          "",
          "Example (**Note**: this is not exhaustive):",
          "",
          "```",
          "[{",
          "    \"container_id\":\"container\",",
          "    \"container_status\":",
          "    {",
          "        \"network_infos\":",
          "        [{\"ip_addresses\":[{\"ip_address\":\"192.168.1.1\"}]}]",
          "    }",
          "    \"executor_id\":\"executor\",",
          "    \"executor_name\":\"name\",",
          "    \"framework_id\":\"framework\",",
          "    \"source\":\"source\",",
          "    \"statistics\":",
          "    {",
          "        \"cpus_limit\":8.25,",
          "        \"cpus_nr_periods\":769021,",
          "        \"cpus_nr_throttled\":1046,",
          "        \"cpus_system_time_secs\":34501.45,",
          "        \"cpus_throttled_time_secs\":352.597023453,",
          "        \"cpus_user_time_secs\":96348.84,",
          "        \"mem_anon_bytes\":4845449216,",
          "        \"mem_file_bytes\":260165632,",
          "        \"mem_limit_bytes\":7650410496,",
          "        \"mem_mapped_file_bytes\":7159808,",
          "        \"mem_rss_bytes\":5105614848,",
          "        \"timestamp\":1388534400.0",
          "    }",
          "}]",
          "```"),
      AUTHENTICATION(true),
      AUTHORIZATION(
          "The request principal should be authorized to query this endpoint.",
          "See the authorization documentation for details."));
}


Future<Response> Slave::Http::containers(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(a10gupta): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Try<string> endpoint = extractEndpoint(request.url);
  if (endpoint.isError()) {
    return Failure("Failed to extract endpoint: " + endpoint.error());
  }

  return authorizeEndpoint(
      endpoint.get(),
      request.method,
      slave->authorizer,
      principal)
    .then(defer(
        slave->self(),
        [this, request, principal](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return _containers(request, principal);
        }));
}


Future<Response> Slave::Http::getContainers(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_CONTAINERS, call.type());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(), [this](
    const Owned<ObjectApprover>& approver) {
        return __containers(approver);
    })).then([acceptType](const Future<JSON::Array>& result)
        -> Future<Response> {
      if (!result.isReady()) {
        LOG(WARNING) << "Could not collect container status and statistics: "
                     << (result.isFailed()
                          ? result.failure()
                          : "Discarded");
        return result.isFailed()
          ? InternalServerError(result.failure())
          : InternalServerError();
      }

      return OK(
          serialize(
              acceptType,
              evolve<v1::agent::Response::GET_CONTAINERS>(result.get())),
          stringify(acceptType));
    });
}


Future<Response> Slave::Http::_containers(
    const Request& request,
    const Option<Principal>& principal) const
{
  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(), [this](
    const Owned<ObjectApprover>& approver) {
      return __containers(approver);
     }))
     .then([request](const Future<JSON::Array>& result) -> Future<Response> {
       if (!result.isReady()) {
         LOG(WARNING) << "Could not collect container status and statistics: "
                      << (result.isFailed()
                           ? result.failure()
                           : "Discarded");

         return result.isFailed()
           ? InternalServerError(result.failure())
           : InternalServerError();
       }

       return process::http::OK(
           result.get(), request.url.query.get("jsonp"));
     });
}


Future<JSON::Array> Slave::Http::__containers(
    Option<Owned<ObjectApprover>> approver) const
{
  Owned<list<JSON::Object>> metadata(new list<JSON::Object>());
  list<Future<ContainerStatus>> statusFutures;
  list<Future<ResourceStatistics>> statsFutures;

  foreachvalue (const Framework* framework, slave->frameworks) {
    foreachvalue (const Executor* executor, framework->executors) {
      // No need to get statistics and status if we know that the
      // executor has already terminated.
      if (executor->state == Executor::TERMINATED) {
        continue;
      }

      const ExecutorInfo& info = executor->info;
      const ContainerID& containerId = executor->containerId;

      Try<bool> authorized = true;

      if (approver.isSome()) {
        ObjectApprover::Object object;
        object.executor_info = &info;
        object.framework_info = &(framework->info);

        authorized = approver.get()->approved(object);

        if (authorized.isError()) {
          LOG(WARNING) << "Error during ViewContainer authorization: "
                       << authorized.error();
          authorized = false;
        }
      }

      if (authorized.get()) {
        JSON::Object entry;
        entry.values["framework_id"] = info.framework_id().value();
        entry.values["executor_id"] = info.executor_id().value();
        entry.values["executor_name"] = info.name();
        entry.values["source"] = info.source();
        entry.values["container_id"] = containerId.value();

        metadata->push_back(entry);
        statusFutures.push_back(slave->containerizer->status(containerId));
        statsFutures.push_back(slave->containerizer->usage(containerId));
      }
    }
  }

  return await(await(statusFutures), await(statsFutures)).then(
      [metadata](const tuple<
          Future<list<Future<ContainerStatus>>>,
          Future<list<Future<ResourceStatistics>>>>& t)
          -> Future<JSON::Array> {
        const list<Future<ContainerStatus>>& status = std::get<0>(t).get();
        const list<Future<ResourceStatistics>>& stats = std::get<1>(t).get();
        CHECK_EQ(status.size(), stats.size());
        CHECK_EQ(status.size(), metadata->size());

        JSON::Array result;

        auto statusIter = status.begin();
        auto statsIter = stats.begin();
        auto metadataIter = metadata->begin();

        while (statusIter != status.end() &&
               statsIter != stats.end() &&
               metadataIter != metadata->end()) {
          JSON::Object& entry= *metadataIter;

          if (statusIter->isReady()) {
            entry.values["status"] = JSON::protobuf(statusIter->get());
          } else {
            LOG(WARNING) << "Failed to get container status for executor '"
                         << entry.values["executor_id"] << "'"
                         << " of framework "
                         << entry.values["framework_id"] << ": "
                         << (statusIter->isFailed()
                              ? statusIter->failure()
                              : "discarded");
          }

          if (statsIter->isReady()) {
            entry.values["statistics"] = JSON::protobuf(statsIter->get());
          } else {
            LOG(WARNING) << "Failed to get resource statistics for executor '"
                         << entry.values["executor_id"] << "'"
                         << " of framework "
                         << entry.values["framework_id"] << ": "
                         << (statsIter->isFailed()
                              ? statsIter->failure()
                              : "discarded");
          }

          result.values.push_back(entry);

          statusIter++;
          statsIter++;
          metadataIter++;
        }

        return result;
      });
}


Try<string> Slave::Http::extractEndpoint(const process::http::URL& url) const
{
  // Paths are of the form "/slave(n)/endpoint". We're only interested
  // in the part after "/slave(n)" and tokenize the path accordingly.
  //
  // TODO(alexr): In the long run, absolute paths for
  // endpoins should be supported, see MESOS-5369.
  const vector<string> pathComponents = strings::tokenize(url.path, "/", 2);

  if (pathComponents.size() < 2u ||
      pathComponents[0] != slave->self().id) {
    return Error("Unexpected path '" + url.path + "'");
  }

  return "/" + pathComponents[1];
}


Future<Response> Slave::Http::readFile(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::READ_FILE, call.type());

  const size_t offset = call.read_file().offset();
  const string& path = call.read_file().path();

  Option<size_t> length;
  if (call.read_file().has_length()) {
    length = call.read_file().length();
  }

  return slave->files->read(offset, length, path, principal)
    .then([acceptType](const Try<tuple<size_t, string>, FilesError>& result)
        -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::READ_FILE);

      response.mutable_read_file()->set_size(std::get<0>(result.get()));
      response.mutable_read_file()->set_data(std::get<1>(result.get()));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    });
}


Future<Response> Slave::Http::launchNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_NESTED_CONTAINER, call.type());
  CHECK(call.has_launch_nested_container());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::LAUNCH_NESTED_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver
    .then(defer(slave->self(), [=](const Owned<ObjectApprover>& approver) {
      return _launchNestedContainer(
          call.launch_nested_container().container_id(),
          call.launch_nested_container().command(),
          call.launch_nested_container().has_container()
            ? call.launch_nested_container().container()
            : Option<ContainerInfo>::none(),
          ContainerClass::DEFAULT,
          acceptType,
          approver);
    }));
}


Future<Response> Slave::Http::_launchNestedContainer(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo,
    const Option<ContainerClass>& containerClass,
    ContentType acceptType,
    const Owned<ObjectApprover>& approver) const
{
  Executor* executor = slave->getExecutor(containerId);
  if (executor == nullptr) {
    return NotFound("Container " + stringify(containerId) + " cannot be found");
  }

  Framework* framework = slave->getFramework(executor->frameworkId);
  CHECK_NOTNULL(framework);

  ObjectApprover::Object object;
  object.executor_info = &(executor->info);
  object.framework_info = &(framework->info);
  object.command_info = &(commandInfo);
  object.container_id = &(containerId);

  Try<bool> approved = approver.get()->approved(object);

  if (approved.isError()) {
    return Failure(approved.error());
  } else if (!approved.get()) {
    return Forbidden();
  }

  // By default, we use the executor's user.
  // The command user overrides it if specified.
  Option<string> user = executor->user;

#ifndef __WINDOWS__
  if (commandInfo.has_user()) {
    user = commandInfo.user();
  }
#endif

  Future<bool> launched = slave->containerizer->launch(
      containerId,
      commandInfo,
      containerInfo,
      user,
      slave->info.id(),
      containerClass);

  // TODO(bmahler): The containerizers currently require that
  // the caller calls destroy if the launch fails. See MESOS-6214.
  launched
    .onFailed(defer(slave->self(), [=](const string& failure) {
      LOG(WARNING) << "Failed to launch nested container " << containerId
                   << ": " << failure;

      slave->containerizer->destroy(containerId)
        .onFailed([=](const string& failure) {
          LOG(ERROR) << "Failed to destroy nested container " << containerId
                     << " after launch failure: " << failure;
        });
    }));

  return launched
    .then([](bool launched) -> Response {
      if (!launched) {
        return BadRequest("The provided ContainerInfo is not supported");
      }
      return OK();
    });
}


Future<Response> Slave::Http::waitNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::WAIT_NESTED_CONTAINER, call.type());
  CHECK(call.has_wait_nested_container());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::WAIT_NESTED_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
    [this, call, acceptType](const Owned<ObjectApprover>& waitApprover)
        -> Future<Response> {
      const ContainerID& containerId =
        call.wait_nested_container().container_id();

      Executor* executor = slave->getExecutor(containerId);
      if (executor == nullptr) {
        return NotFound(
            "Container " + stringify(containerId) + " cannot be found");
      }

      Framework* framework = slave->getFramework(executor->frameworkId);
      CHECK_NOTNULL(framework);

      ObjectApprover::Object object;
      object.executor_info = &(executor->info);
      object.framework_info = &(framework->info);
      object.container_id = &(containerId);

      Try<bool> approved = waitApprover.get()->approved(object);

      if (approved.isError()) {
        return Failure(approved.error());
      } else if (!approved.get()) {
        return Forbidden();
      }

      Future<Option<mesos::slave::ContainerTermination>> wait =
        slave->containerizer->wait(containerId);

      return wait
        .then([containerId, acceptType](
            const Option<ContainerTermination>& termination) -> Response {
          if (termination.isNone()) {
            return NotFound("Container " + stringify(containerId) +
                            " cannot be found");
          }

          mesos::agent::Response response;
          response.set_type(mesos::agent::Response::WAIT_NESTED_CONTAINER);

          mesos::agent::Response::WaitNestedContainer* waitNestedContainer =
            response.mutable_wait_nested_container();

          if (termination->has_status()) {
            waitNestedContainer->set_exit_status(termination->status());
          }

          return OK(serialize(acceptType, evolve(response)),
                    stringify(acceptType));
        });
    }));
}


Future<Response> Slave::Http::killNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::KILL_NESTED_CONTAINER, call.type());
  CHECK(call.has_kill_nested_container());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::KILL_NESTED_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
    [this, call](const Owned<ObjectApprover>& killApprover)
        -> Future<Response> {
      const ContainerID& containerId =
        call.kill_nested_container().container_id();

      Executor* executor = slave->getExecutor(containerId);
      if (executor == nullptr) {
        return NotFound(
            "Container " + stringify(containerId) + " cannot be found");
      }

      Framework* framework = slave->getFramework(executor->frameworkId);
      CHECK_NOTNULL(framework);

      ObjectApprover::Object object;
      object.executor_info = &(executor->info);
      object.framework_info = &(framework->info);
      object.container_id = &(containerId);

      Try<bool> approved = killApprover.get()->approved(object);

      if (approved.isError()) {
        return Failure(approved.error());
      } else if (!approved.get()) {
        return Forbidden();
      }

      Future<bool> destroy = slave->containerizer->destroy(containerId);

      return destroy
        .then([containerId](bool found) -> Response {
          if (!found) {
            return NotFound("Container '" + stringify(containerId) + "'"
                            " cannot be found (or is already killed)");
          }
          return OK();
        });
    }));
}


Future<Response> Slave::Http::removeNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::REMOVE_NESTED_CONTAINER, call.type());
  CHECK(call.has_remove_nested_container());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::REMOVE_NESTED_CONTAINER);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
    [this, call](const Owned<ObjectApprover>& removeApprover)
        -> Future<Response> {
      const ContainerID& containerId =
        call.remove_nested_container().container_id();

      Executor* executor = slave->getExecutor(containerId);
      if (executor == nullptr) {
        return OK();
      }

      Framework* framework = slave->getFramework(executor->frameworkId);
      CHECK_NOTNULL(framework);

      ObjectApprover::Object object;
      object.executor_info = &(executor->info);
      object.framework_info = &(framework->info);
      object.container_id = &(containerId);

      Try<bool> approved = removeApprover.get()->approved(object);

      if (approved.isError()) {
        return Failure(approved.error());
      } else if (!approved.get()) {
        return Forbidden();
      }

      Future<Nothing> remove = slave->containerizer->remove(containerId);

      return remove.then(
          [containerId](const Future<Nothing>& result) -> Response {
            if (result.isFailed()) {
              LOG(ERROR) << "Failed to remove nested container " << containerId
                         << ": " << result.failure();
              return InternalServerError(result.failure());
            }

            return OK();
          });
    }));
}


Future<Response> Slave::Http::_attachContainerInput(
    const mesos::agent::Call& call,
    Owned<Reader<mesos::agent::Call>>&& decoder,
    const RequestMediaTypes& mediaTypes) const
{
  const ContainerID& containerId = call.attach_container_input().container_id();

  Pipe pipe;
  Pipe::Reader reader = pipe.reader();
  Pipe::Writer writer = pipe.writer();

  CHECK_SOME(mediaTypes.messageContent);
  auto encoder = [mediaTypes](const mesos::agent::Call& call) {
    ::recordio::Encoder<mesos::agent::Call> encoder(lambda::bind(
        serialize, mediaTypes.messageContent.get(), lambda::_1));

    return encoder.encode(call);
  };

  // Write the first record. We had extracted it from the `decoder`
  // in the `api()` handler to identify the call type earlier.
  pipe.writer().write(encoder(call));

  // We create this here since C++11 does not support move capture of `reader`.
  Future<Nothing> transform = recordio::transform<mesos::agent::Call>(
      std::move(decoder), encoder, writer);

  return slave->containerizer->attach(containerId)
    .then([mediaTypes, reader, writer, transform](
        Connection connection) mutable {
      Request request;
      request.method = "POST";
      request.type = Request::PIPE;
      request.reader = reader;
      request.headers = {{"Content-Type", stringify(mediaTypes.content)},
                         {MESSAGE_CONTENT_TYPE,
                             stringify(mediaTypes.messageContent.get())},
                         {"Accept", stringify(mediaTypes.accept)}};

      // See comments in `attachContainerOutput()` for the reasoning
      // behind these values.
      request.url.domain = "";
      request.url.path = "/";

      transform
        .onAny([writer](
            const Future<Nothing>& future) mutable {
          CHECK(!future.isDiscarded());

          if (future.isFailed()) {
            writer.fail(future.failure());
            return;
          }

          writer.close();
         });

      // This is a non Keep-Alive request which means the connection
      // will be closed when the response is received. Since the
      // 'Connection' is reference-counted, we must maintain a copy
      // until the disconnection occurs.
      connection.disconnected()
        .onAny([connection]() {});

      return connection.send(request);
    });
}


Future<Response> Slave::Http::attachContainerInput(
    const mesos::agent::Call& call,
    Owned<Reader<mesos::agent::Call>>&& decoder,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::ATTACH_CONTAINER_INPUT, call.type());
  CHECK(call.has_attach_container_input());

  if (call.attach_container_input().type() !=
      mesos::agent::Call::AttachContainerInput::CONTAINER_ID) {
    return BadRequest(
        "Expecting 'attach_container_input.type' to be CONTAINER_ID");
  }

  CHECK(call.attach_container_input().has_container_id());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::ATTACH_CONTAINER_INPUT);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
    [this, call, decoder, mediaTypes](
        const Owned<ObjectApprover>& attachInputApprover) -> Future<Response> {
      const ContainerID& containerId =
        call.attach_container_input().container_id();

      Executor* executor = slave->getExecutor(containerId);
      if (executor == nullptr) {
        return NotFound(
            "Container " + stringify(containerId) + " cannot be found");
      }

      Framework* framework = slave->getFramework(executor->frameworkId);
      CHECK_NOTNULL(framework);

      ObjectApprover::Object object;
      object.executor_info = &(executor->info);
      object.framework_info = &(framework->info);

      Try<bool> approved = attachInputApprover.get()->approved(object);

      if (approved.isError()) {
        return Failure(approved.error());
      } else if (!approved.get()) {
        return Forbidden();
      }

      Owned<Reader<mesos::agent::Call>> decoder_ = decoder;

      return _attachContainerInput(
          call, std::move(decoder_), mediaTypes);
  }));
}


// Helper that reads data from `writer` and writes to `reader`.
// Returns a failed future if there are any errors reading or writing.
// The future is satisfied when we get a EOF.
// TODO(vinod): Move this to libprocess if this is more generally useful.
Future<Nothing> connect(Pipe::Reader reader, Pipe::Writer writer)
{
  return loop(
      None(),
      [=]() mutable {
        return reader.read();
      },
      [=](const string& chunk) mutable -> Future<ControlFlow<Nothing>> {
        if (chunk.empty()) {
          // EOF case.
          return Break();
        }

        if (!writer.write(chunk)) {
          return Failure("Write failed to the pipe");
        }

        return Continue();
      });
}


Future<Response> Slave::Http::launchNestedContainerSession(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION, call.type());
  CHECK(call.has_launch_nested_container_session());

  const ContainerID& containerId =
    call.launch_nested_container_session().container_id();

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::LAUNCH_NESTED_CONTAINER_SESSION);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  Future<Response> response = approver
    .then(defer(slave->self(), [=](const Owned<ObjectApprover>& approver) {
      return _launchNestedContainer(
          call.launch_nested_container_session().container_id(),
          call.launch_nested_container_session().command(),
          call.launch_nested_container_session().has_container()
            ? call.launch_nested_container_session().container()
            : Option<ContainerInfo>::none(),
          ContainerClass::DEBUG,
          mediaTypes.accept,
          approver);
    }));

  // Helper to destroy the container.
  auto destroy = [this](const ContainerID& containerId) {
    slave->containerizer->destroy(containerId)
      .onFailed([containerId](const string& failure) {
        LOG(ERROR) << "Failed to destroy nested container "
                   << containerId << ": " << failure;
      });
  };

  // If `response` has failed or is not `OK`, the container will be
  // destroyed by `_launchNestedContainer`.
  return response
    .then(defer(slave->self(),
                [=](const Response& response) -> Future<Response> {
      if (response.status != OK().status) {
        return response;
      }

      // If launch is successful, attach to the container output.
      mesos::agent::Call call;
      call.set_type(mesos::agent::Call::ATTACH_CONTAINER_OUTPUT);
      call.mutable_attach_container_output()->mutable_container_id()
          ->CopyFrom(containerId);

      // Instead of directly returning the response of `attachContainerOutput`
      // to the client, we use a level of indirection to make sure the container
      // is destroyed when the client connection breaks.
      return attachContainerOutput(call, mediaTypes, principal)
        .then(defer(slave->self(),
                    [=](const Response& response) -> Future<Response> {
          if (response.status != OK().status) {
            return response;
          }

          Pipe pipe;
          Pipe::Writer writer = pipe.writer();

          OK ok;
          ok.headers = response.headers; // Reuse the headers from the response.
          ok.type = Response::PIPE;
          ok.reader = pipe.reader();

          CHECK_EQ(Response::PIPE, response.type);
          CHECK_SOME(response.reader);
          Pipe::Reader reader = response.reader.get();

          // Read from the `response` pipe and write to
          // the client's response pipe.
          // NOTE: Need to cast the lambda to std::function here because of a
          // limitation of `defer`; `defer` does not work with `mutable` lambda.
          std::function<void (const Future<Nothing>&)> _connect =
            [=](const Future<Nothing>& future) mutable {
              CHECK(!future.isDiscarded());

              if (future.isFailed()) {
                LOG(WARNING) << "Failed to send attach response for "
                             << containerId << ": " << future.failure();

                writer.fail(future.failure());
                reader.close();
              } else {
                // EOF case.
                LOG(INFO) << "Received EOF attach response for " << containerId;

                writer.close();
                reader.close();
              }

              destroy(containerId);
          };

          connect(reader, writer)
            .onAny(defer(slave->self(), _connect));

          // Destroy the container if the connection to client is closed.
          writer.readerClosed()
            .onAny(defer(slave->self(), [=](const Future<Nothing>& future) {
              LOG(WARNING)
                << "Launch nested container session connection"
                << " for container " << containerId << " closed"
                << (future.isFailed() ? ": " + future.failure() : "");

              destroy(containerId);
            }));

          return ok;
        }))
        .onFailed(defer(slave->self(), [=](const string& failure) {
          LOG(WARNING) << "Failed to attach to nested container "
                       << containerId << ": " << failure;

          destroy(containerId);
        }));
    }));
}


Future<Response> Slave::Http::_attachContainerOutput(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes) const
{
  const ContainerID& containerId =
    call.attach_container_output().container_id();

  return slave->containerizer->attach(containerId)
    .then([call, mediaTypes](Connection connection)
        -> Future<Response> {
      Request request;
      request.method = "POST";
      request.headers = {{"Accept", stringify(mediaTypes.accept)},
                         {"Content-Type", stringify(mediaTypes.content)}};

      // If a client sets the 'Accept' header expecting a streaming response,
      // `messageAccept` would always be set and we use it as the value of
      // 'Message-Accept' header.
      if (streamingMediaType(mediaTypes.accept)) {
        CHECK_SOME(mediaTypes.messageAccept);
        request.headers[MESSAGE_ACCEPT] =
          stringify(mediaTypes.messageAccept.get());
      }

      // The 'HOST' header must be EMPTY for non Internet addresses.
      // TODO(vinod): Instead of setting domain to empty string (which results
      // in an empty HOST header), add a new URL constructor that doesn't
      // require domain or IP.
      request.url.domain = "";

      // NOTE: The path is currently ignored by the switch board.
      request.url.path = "/";

      request.type = Request::BODY;
      request.body = serialize(mediaTypes.content, call);

      // We capture `connection` here to ensure that it doesn't go
      // out of scope until the `onAny` handler on `transform` is executed.
      return connection.send(request, true)
        .then([connection, mediaTypes](const Response& response)
            -> Future<Response> {
          if (response.status != OK().status) {
            return response;
          }

          // Evolve the `ProcessIO` records in the Response body to v1
          // before sending them to the client.
          Pipe pipe;
          Pipe::Writer writer = pipe.writer();

          OK ok;
          ok.headers = response.headers; // Reuse headers from response.

          // If a client sets the 'Accept' header expecting a streaming
          // response, `messageAccept` would always be set and we use it
          // to deserialize/evolve messages in the streaming response.
          ContentType messageContentType = mediaTypes.accept;
          if (streamingMediaType(mediaTypes.accept)) {
            CHECK_SOME(mediaTypes.messageAccept);
            messageContentType = mediaTypes.messageAccept.get();
          }

          ok.type = Response::PIPE;
          ok.reader = pipe.reader();

          CHECK_EQ(Response::PIPE, response.type);
          CHECK_SOME(response.reader);
          Pipe::Reader reader = response.reader.get();

          auto deserializer = lambda::bind(
              deserialize<ProcessIO>, messageContentType, lambda::_1);

          Owned<Reader<ProcessIO>> decoder(new Reader<ProcessIO>(
              Decoder<ProcessIO>(deserializer), reader));

          auto encoder = [messageContentType](const ProcessIO& processIO) {
            ::recordio::Encoder<v1::agent::ProcessIO> encoder (lambda::bind(
                serialize, messageContentType, lambda::_1));

            return encoder.encode(evolve(processIO));
          };

          recordio::transform<ProcessIO>(std::move(decoder), encoder, writer)
            .onAny([writer, reader, connection](
                const Future<Nothing>& future) mutable {
              CHECK(!future.isDiscarded());

              if (future.isFailed()) {
                writer.fail(future.failure());
                reader.close();
                return;
              }

              writer.close();
              reader.close();
            });

          return ok;
        });
    });
}


Future<Response> Slave::Http::attachContainerOutput(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::ATTACH_CONTAINER_OUTPUT, call.type());
  CHECK(call.has_attach_container_output());

  Future<Owned<ObjectApprover>> approver;

  if (slave->authorizer.isSome()) {
    Option<authorization::Subject> subject = createSubject(principal);

    approver = slave->authorizer.get()->getObjectApprover(
        subject, authorization::ATTACH_CONTAINER_OUTPUT);
  } else {
    approver = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return approver.then(defer(slave->self(),
    [this, call, mediaTypes](
        const Owned<ObjectApprover>& attachOutputApprover) -> Future<Response> {
      const ContainerID& containerId =
        call.attach_container_output().container_id();

      Executor* executor = slave->getExecutor(containerId);
      if (executor == nullptr) {
        return NotFound(
            "Container " + stringify(containerId) + " cannot be found");
      }

      Framework* framework = slave->getFramework(executor->frameworkId);
      CHECK_NOTNULL(framework);

      ObjectApprover::Object object;
      object.executor_info = &(executor->info);
      object.framework_info = &(framework->info);
      object.container_id = &(containerId);

      Try<bool> approved = attachOutputApprover.get()->approved(object);

      if (approved.isError()) {
        return Failure(approved.error());
      } else if (!approved.get()) {
        return Forbidden();
      }

      return _attachContainerOutput(call, mediaTypes);
  }));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
