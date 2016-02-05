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

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <mesos/executor/executor.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <process/help.hpp>
#include <process/owned.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "common/build.hpp"
#include "common/http.hpp"

#include "internal/devolve.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/slave.hpp"
#include "slave/validation.hpp"


using process::Clock;
using process::DESCRIPTION;
using process::Future;
using process::HELP;
using process::Owned;
using process::TLDR;
using process::USAGE;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Forbidden;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::UnsupportedMediaType;

using process::metrics::internal::MetricsProcess;

using std::map;
using std::string;
using std::vector;


namespace mesos {
namespace internal {
namespace slave {


// Pull in defnitions from common.
using mesos::internal::model;

// Pull in the process definitions.
using process::http::Response;
using process::http::Request;


JSON::Object model(const TaskInfo& task)
{
  JSON::Object object;
  object.values["id"] = task.task_id().value();
  object.values["name"] = task.name();
  object.values["slave_id"] = task.slave_id().value();
  object.values["resources"] = model(task.resources());

  if (task.has_command()) {
    object.values["command"] = model(task.command());
  }
  if (task.has_executor()) {
    object.values["executor_id"] = task.executor().executor_id().value();
  }
  if (task.has_discovery()) {
    object.values["discovery"] = JSON::protobuf(task.discovery());
  }

  return object;
}


JSON::Object model(const Executor& executor)
{
  JSON::Object object;
  object.values["id"] = executor.id.value();
  object.values["name"] = executor.info.name();
  object.values["source"] = executor.info.source();
  object.values["container"] = executor.containerId.value();
  object.values["directory"] = executor.directory;
  object.values["resources"] = model(executor.resources);

  JSON::Array tasks;
  foreach (Task* task, executor.launchedTasks.values()) {
    tasks.values.push_back(model(*task));
  }
  object.values["tasks"] = tasks;

  JSON::Array queued;
  foreach (const TaskInfo& task, executor.queuedTasks.values()) {
    queued.values.push_back(model(task));
  }
  object.values["queued_tasks"] = queued;

  JSON::Array completed;
  foreach (const std::shared_ptr<Task>& task, executor.completedTasks) {
    completed.values.push_back(model(*task));
  }

  // NOTE: We add 'terminatedTasks' to 'completed_tasks' for
  // simplicity.
  // TODO(vinod): Use foreachvalue instead once LinkedHashmap
  // supports it.
  foreach (Task* task, executor.terminatedTasks.values()) {
    completed.values.push_back(model(*task));
  }
  object.values["completed_tasks"] = completed;

  return object;
}


// Returns a JSON object modeled after a Framework.
JSON::Object model(const Framework& framework)
{
  JSON::Object object;
  object.values["id"] = framework.id().value();
  object.values["name"] = framework.info.name();
  object.values["user"] = framework.info.user();
  object.values["failover_timeout"] = framework.info.failover_timeout();
  object.values["checkpoint"] = framework.info.checkpoint();
  object.values["role"] = framework.info.role();
  object.values["hostname"] = framework.info.hostname();

  JSON::Array executors;
  foreachvalue (Executor* executor, framework.executors) {
    executors.values.push_back(model(*executor));
  }
  object.values["executors"] = executors;

  JSON::Array completedExecutors;
  foreach (const Owned<Executor>& executor, framework.completedExecutors) {
    completedExecutors.values.push_back(model(*executor));
  }
  object.values["completed_executors"] = completedExecutors;

  return object;
}


void Slave::Http::log(const Request& request)
{
  Option<string> userAgent = request.headers.get("User-Agent");
  Option<string> forwardedFor = request.headers.get("X-Forwarded-For");

  LOG(INFO) << "HTTP " << request.method << " for " << request.url.path
            << " from " << request.client
            << (userAgent.isSome()
                ? " with User-Agent='" + userAgent.get() + "'"
                : "")
            << (forwardedFor.isSome()
                ? " with X-Forwarded-For='" + forwardedFor.get() + "'"
                : "");
}


string Slave::Http::EXECUTOR_HELP() {
  return HELP(
    TLDR(
        "Endpoint for the Executor HTTP API."),
    DESCRIPTION(
        "This endpoint is used by the executors to interact with the",
        "agent via Call/Event messages.",
        "Returns 200 OK iff the initial SUBSCRIBE Call is successful.",
        "This would result in a streaming response via chunked",
        "transfer encoding. The executors can process the response",
        "incrementally.",
        "Returns 202 Accepted for all other Call messages iff the",
        "request is accepted."));
}


Future<Response> Slave::Http::executor(const Request& request) const
{
  // TODO(anand): Add metrics for rejected requests.

  if (request.method != "POST") {
    return MethodNotAllowed(
        {"POST"}, "Expecting 'POST', received '" + request.method + "'");
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

  ContentType responseContentType;

  if (call.type() == executor::Call::SUBSCRIBE) {
    // We default to JSON since an empty 'Accept' header
    // results in all media types considered acceptable.
    if (request.acceptsMediaType(APPLICATION_JSON)) {
      responseContentType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      responseContentType = ContentType::PROTOBUF;
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
  if (framework == NULL) {
    return BadRequest("Framework cannot be found");
  }

  Executor* executor = framework->getExecutor(call.executor_id());
  if (executor == NULL) {
    return BadRequest("Executor cannot be found");
  }

  if (executor->state == Executor::REGISTERING &&
      call.type() != executor::Call::SUBSCRIBE) {
    return Forbidden("Executor is not subscribed");
  }

  switch (call.type()) {
    case executor::Call::SUBSCRIBE: {
      Pipe pipe;
      OK ok;
      ok.headers["Content-Type"] = stringify(responseContentType);

      ok.type = Response::PIPE;
      ok.reader = pipe.reader();

      HttpConnection http {pipe.writer(), responseContentType};
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

    default:
      // Should be caught during call validation above.
      LOG(FATAL) << "Unexpected " << call.type() << " call";
  }

  return NotImplemented();
}


string Slave::Http::FLAGS_HELP()
{
  return HELP(TLDR("Exposes the agent's flag configuration."));
}


Future<Response> Slave::Http::flags(const Request& request) const
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachpair (const string& name, const flags::Flag& flag, slave->flags) {
      Option<string> value = flag.stringify(slave->flags);
      if (value.isSome()) {
        flags.values[name] = value.get();
      }
    }
    object.values["flags"] = std::move(flags);
  }

  return OK(object, request.url.query.get("jsonp"));
}


string Slave::Http::HEALTH_HELP()
{
  return HELP(
    TLDR(
        "Health check of the Slave."),
    DESCRIPTION(
        "Returns 200 OK iff the Slave is healthy.",
        "Delayed responses are also indicative of poor health."));
}


Future<Response> Slave::Http::health(const Request& request) const
{
  return OK();
}


string Slave::Http::STATE_HELP() {
  return HELP(
    TLDR(
        "Information about state of the Slave."),
    DESCRIPTION(
        "This endpoint shows information about the frameworks, executors",
        "and the slave's master as a JSON object."));
}


Future<Response> Slave::Http::state(const Request& request) const
{
  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  JSON::Object object;
  object.values["version"] = MESOS_VERSION;

  if (build::GIT_SHA.isSome()) {
    object.values["git_sha"] = build::GIT_SHA.get();
  }

  if (build::GIT_BRANCH.isSome()) {
    object.values["git_branch"] = build::GIT_BRANCH.get();
  }

  if (build::GIT_TAG.isSome()) {
    object.values["git_tag"] = build::GIT_TAG.get();
  }

  object.values["build_date"] = build::DATE;
  object.values["build_time"] = build::TIME;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = slave->startTime.secs();
  object.values["id"] = slave->info.id().value();
  object.values["pid"] = string(slave->self());
  object.values["hostname"] = slave->info.hostname();
  object.values["resources"] = model(slave->info.resources());
  object.values["attributes"] = model(slave->info.attributes());

  if (slave->master.isSome()) {
    Try<string> hostname = net::getHostname(slave->master.get().address.ip);
    if (hostname.isSome()) {
      object.values["master_hostname"] = hostname.get();
    }
  }

  if (slave->flags.log_dir.isSome()) {
    object.values["log_dir"] = slave->flags.log_dir.get();
  }

  if (slave->flags.external_log_file.isSome()) {
    object.values["external_log_file"] = slave->flags.external_log_file.get();
  }

  JSON::Array frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    frameworks.values.push_back(model(*framework));
  }
  object.values["frameworks"] = frameworks;

  JSON::Array completedFrameworks;
  foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
    completedFrameworks.values.push_back(model(*framework));
  }
  object.values["completed_frameworks"] = completedFrameworks;

  JSON::Object flags;
  foreachpair (const string& name, const flags::Flag& flag, slave->flags) {
    Option<string> value = flag.stringify(slave->flags);
    if (value.isSome()) {
      flags.values[name] = value.get();
    }
  }
  object.values["flags"] = flags;

  return OK(object, request.url.query.get("jsonp"));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
