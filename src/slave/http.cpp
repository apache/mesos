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

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/collect.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/logging.hpp>
#include <process/limiter.hpp>
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

#include "internal/devolve.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/slave.hpp"
#include "slave/validation.hpp"

#include "version/version.hpp"

using mesos::slave::ContainerTermination;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::Clock;
using process::DESCRIPTION;
using process::Future;
using process::HELP;
using process::Logging;
using process::Owned;
using process::TLDR;

using process::http::Accepted;
using process::http::BadRequest;
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

using process::metrics::internal::MetricsProcess;

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

    if (executor_->info.has_labels()) {
      writer->field("labels", executor_->info.labels());
    }

    writer->field("tasks", [this](JSON::ArrayWriter* writer) {
      foreach (Task* task, executor_->launchedTasks.values()) {
        if (!approveViewTask(taskApprover_, *task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });

    writer->field("queued_tasks", [this](JSON::ArrayWriter* writer) {
      foreach (const TaskInfo& task, executor_->queuedTasks.values()) {
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
      // TODO(vinod): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (Task* task, executor_->terminatedTasks.values()) {
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
    writer->field("role", framework_->info.role());
    writer->field("hostname", framework_->info.hostname());

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
    const Option<string>& principal) const
{
  // TODO(anand): Add metrics for rejected requests.

  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::agent::Call v1Call;

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

    Try<v1::agent::Call> parse =
      ::protobuf::parse<v1::agent::Call>(value.get());

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

  agent::Call call = devolve(v1Call);

  Option<Error> error = validation::agent::call::validate(call);

  if (error.isSome()) {
    return BadRequest("Failed to validate agent::Call: " + error.get().message);
  }

  LOG(INFO) << "Processing call " << call.type();

  ContentType acceptType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    acceptType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    acceptType = ContentType::PROTOBUF;
  } else {
    return NotAcceptable(
        string("Expecting 'Accept' to allow ") +
        "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
  }

  switch (call.type()) {
    case agent::Call::UNKNOWN:
      return NotImplemented();

    case agent::Call::GET_HEALTH:
      return getHealth(call, principal, acceptType);

    case agent::Call::GET_FLAGS:
      return getFlags(call, principal, acceptType);

    case agent::Call::GET_VERSION:
      return getVersion(call, principal, acceptType);

    case agent::Call::GET_METRICS:
      return getMetrics(call, principal, acceptType);

    case agent::Call::GET_LOGGING_LEVEL:
      return getLoggingLevel(call, principal, acceptType);

    case agent::Call::SET_LOGGING_LEVEL:
      return setLoggingLevel(call, principal, acceptType);

    case agent::Call::LIST_FILES:
      return listFiles(call, principal, acceptType);

    case agent::Call::READ_FILE:
      return readFile(call, principal, acceptType);

    case agent::Call::GET_STATE:
      return getState(call, principal, acceptType);

    case agent::Call::GET_CONTAINERS:
      return getContainers(call, principal, acceptType);

    case agent::Call::GET_FRAMEWORKS:
      return getFrameworks(call, principal, acceptType);

    case agent::Call::GET_EXECUTORS:
      return getExecutors(call, principal, acceptType);

    case agent::Call::GET_TASKS:
      return getTasks(call, principal, acceptType);

    case agent::Call::LAUNCH_NESTED_CONTAINER:
      return launchNestedContainer(call, principal, acceptType);

    case agent::Call::WAIT_NESTED_CONTAINER:
      return waitNestedContainer(call, principal, acceptType);

    case agent::Call::KILL_NESTED_CONTAINER:
      return killNestedContainer(call, principal, acceptType);
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
        "Returns 200 OK iff the initial SUBSCRIBE Call is successful.",
        "This would result in a streaming response via chunked",
        "transfer encoding. The executors can process the response",
        "incrementally.",
        "Returns 202 Accepted for all other Call messages iff the",
        "request is accepted."),
    AUTHENTICATION(false));
}


Future<Response> Slave::Http::executor(const Request& request) const
{
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
    const Option<string>& principal) const
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

  if (principal.isSome()) {
    authRequest.mutable_subject()->set_value(principal.get());
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
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_FLAGS, call.type());

  return OK(serialize(contentType,
                      evolve<v1::agent::Response::GET_FLAGS>(_flags())),
            stringify(contentType));
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
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_HEALTH, call.type());

  agent::Response response;
  response.set_type(agent::Response::GET_HEALTH);
  response.mutable_get_health()->set_healthy(true);

  return OK(serialize(contentType, evolve(response)),
            stringify(contentType));
}


Future<Response> Slave::Http::getVersion(
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_VERSION, call.type());

  return OK(serialize(contentType,
                      evolve<v1::agent::Response::GET_VERSION>(version())),
            stringify(contentType));
}


Future<Response> Slave::Http::getMetrics(
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_METRICS, call.type());
  CHECK(call.has_get_metrics());

  Option<Duration> timeout;
  if (call.get_metrics().has_timeout()) {
    timeout = Nanoseconds(call.get_metrics().timeout().nanoseconds());
  }

  return process::metrics::snapshot(timeout)
      .then([contentType](const hashmap<string, double>& metrics) -> Response {
        agent::Response response;
        response.set_type(agent::Response::GET_METRICS);
        agent::Response::GetMetrics* _getMetrics =
          response.mutable_get_metrics();

        foreachpair (const string& key, double value, metrics) {
          Metric* metric = _getMetrics->add_metrics();
          metric->set_name(key);
          metric->set_value(value);
        }

        return OK(serialize(contentType, evolve(response)),
                  stringify(contentType));
      });
}


Future<Response> Slave::Http::getLoggingLevel(
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_LOGGING_LEVEL, call.type());

  agent::Response response;
  response.set_type(agent::Response::GET_LOGGING_LEVEL);
  response.mutable_get_logging_level()->set_level(FLAGS_v);

  return OK(serialize(contentType, evolve(response)),
            stringify(contentType));
}


Future<Response> Slave::Http::setLoggingLevel(
    const agent::Call& call,
    const Option<string>& principal,
    ContentType /*contentType*/) const
{
  CHECK_EQ(agent::Call::SET_LOGGING_LEVEL, call.type());
  CHECK(call.has_set_logging_level());

  uint32_t level = call.set_logging_level().level();
  Duration duration =
    Nanoseconds(call.set_logging_level().duration().nanoseconds());

  return dispatch(process::logging(), &Logging::set_level, level, duration)
      .then([]() -> Response {
        return OK();
      });
}


Future<Response> Slave::Http::listFiles(
    const mesos::agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::agent::Call::LIST_FILES, call.type());

  const string& path = call.list_files().path();

  return slave->files->browse(path, principal)
    .then([contentType](const Try<list<FileInfo>, FilesError>& result)
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

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
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
    const Option<string>& principal) const
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
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

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
          foreach (const Owned<Framework>& framework,
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
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_FRAMEWORKS, call.type());

  // Retrieve `ObjectApprover`s for authorizing frameworks.
  Future<Owned<ObjectApprover>> frameworksApprover;

  if (slave->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

    frameworksApprover = slave->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return frameworksApprover
    .then(defer(slave->self(),
        [this, contentType](const Owned<ObjectApprover>& frameworksApprover)
          -> Future<Response> {
      agent::Response response;
      response.set_type(agent::Response::GET_FRAMEWORKS);
      response.mutable_get_frameworks()->CopyFrom(
          _getFrameworks(frameworksApprover));

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    }));
}


agent::Response::GetFrameworks Slave::Http::_getFrameworks(
    const Owned<ObjectApprover>& frameworksApprover) const
{
  agent::Response::GetFrameworks getFrameworks;
  foreachvalue (const Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    getFrameworks.add_frameworks()->mutable_framework_info()
      ->CopyFrom(framework->info);
  }

  foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
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
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_EXECUTORS, call.type());

  // Retrieve `ObjectApprover`s for authorizing frameworks and executors.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

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
        [this, contentType](const tuple<Owned<ObjectApprover>,
                                        Owned<ObjectApprover>>& approvers)
          -> Future<Response> {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, executorsApprover) = approvers;

      agent::Response response;
      response.set_type(agent::Response::GET_EXECUTORS);

      response.mutable_get_executors()->CopyFrom(
          _getExecutors(frameworksApprover, executorsApprover));

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    }));
}


agent::Response::GetExecutors Slave::Http::_getExecutors(
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

  foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  agent::Response::GetExecutors getExecutors;

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
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_TASKS, call.type());

  // Retrieve Approvers for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

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
      [this, contentType](const tuple<Owned<ObjectApprover>,
                                      Owned<ObjectApprover>,
                                      Owned<ObjectApprover>>& approvers)
        -> Future<Response> {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> tasksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, tasksApprover, executorsApprover) = approvers;

      agent::Response response;
      response.set_type(agent::Response::GET_TASKS);

      response.mutable_get_tasks()->CopyFrom(
          _getTasks(frameworksApprover,
                    tasksApprover,
                    executorsApprover));

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
  }));
}


agent::Response::GetTasks Slave::Http::_getTasks(
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

  foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
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

  agent::Response::GetTasks getTasks;

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
    foreach (const TaskInfo& taskInfo, executor->queuedTasks.values()) {
      // Skip unauthorized tasks.
      if (!approveViewTaskInfo(tasksApprover, taskInfo, framework->info)) {
        continue;
      }

      const Task& task =
        protobuf::createTask(taskInfo, TASK_STAGING, framework->id());

      getTasks.add_queued_tasks()->CopyFrom(task);
    }

    // Launched tasks.
    foreach (Task* task, executor->launchedTasks.values()) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approveViewTask(tasksApprover, *task, framework->info)) {
        continue;
      }

      getTasks.add_launched_tasks()->CopyFrom(*task);
    }

    // Terminated tasks.
    foreach (Task* task, executor->terminatedTasks.values()) {
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


Future<Response> Slave::Http::getState(
    const agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_STATE, call.type());

  // Retrieve Approvers for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (slave->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

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

      agent::Response response;
      response.set_type(agent::Response::GET_STATE);
      response.mutable_get_state()->CopyFrom(
          _getState(frameworksApprover,
                    tasksApprover,
                    executorsApprover));

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    }));
}


agent::Response::GetState Slave::Http::_getState(
    const Owned<ObjectApprover>& frameworksApprover,
    const Owned<ObjectApprover>& tasksApprover,
    const Owned<ObjectApprover>& executorsApprover) const
{
  agent::Response::GetState getState;

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
    const Option<string>& principal) const
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
      const ExecutorInfo info = executor.executor_info();

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
    const Option<string>& principal) const
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
        [this, request](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return _containers(request);
        }));
}


Future<Response> Slave::Http::getContainers(
    const agent::Call& call,
    const Option<string>& printcipal,
    ContentType contentType) const
{
  CHECK_EQ(agent::Call::GET_CONTAINERS, call.type());

  return __containers()
      .then([contentType](const Future<JSON::Array>& result)
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
                contentType,
                evolve<v1::agent::Response::GET_CONTAINERS>(result.get())),
            stringify(contentType));
      });
}


Future<Response> Slave::Http::_containers(const Request& request) const
{
  return __containers()
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


Future<JSON::Array> Slave::Http::__containers() const
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
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::agent::Call::READ_FILE, call.type());

  const size_t offset = call.read_file().offset();
  const string& path = call.read_file().path();

  Option<size_t> length;
  if (call.read_file().has_length()) {
    length = call.read_file().length();
  }

  return slave->files->read(offset, length, path, principal)
    .then([contentType](const Try<tuple<size_t, string>, FilesError>& result)
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

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


Future<Response> Slave::Http::launchNestedContainer(
    const mesos::agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_NESTED_CONTAINER, call.type());
  CHECK(call.has_launch_nested_container());

  const ContainerID& containerId =
    call.launch_nested_container().container_id();

  // We do not yet support launching containers that are nested
  // two levels beneath the executor's container.
  if (containerId.parent().has_parent()) {
    return NotImplemented(
        "Only a single level of container nesting is supported currently,"
        " but 'launch_nested_container.container_id.parent.parent' is set");
  }

  // TODO(gilbert): The sandbox directory and user are incorrect,
  // Please update it.
  Future<bool> launched = slave->containerizer->launch(
      containerId,
      call.launch_nested_container().command(),
      call.launch_nested_container().has_container()
        ? call.launch_nested_container().container()
        : Option<ContainerInfo>::none(),
      None(),
      slave->info.id());

  // TODO(bmahler): The containerizers currently require that
  // the caller calls destroy if the launch fails. See MESOS-6214.
  launched
    .onFailed(defer(slave->self(), [=](const string& failure) {
      LOG(WARNING) << "Failed to launch nested container " << containerId
                   << ": " << failure;

      slave->containerizer->destroy(containerId)
        .onFailed([=](const string& failure) {
          LOG(ERROR) << "Failed to destroy neseted container " << containerId
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
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::agent::Call::WAIT_NESTED_CONTAINER, call.type());
  CHECK(call.has_wait_nested_container());

  const ContainerID& containerId =
    call.wait_nested_container().container_id();

  Future<Option<mesos::slave::ContainerTermination>> wait =
    slave->containerizer->wait(containerId);

  return wait
    .then([containerId, contentType](
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

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


Future<Response> Slave::Http::killNestedContainer(
    const mesos::agent::Call& call,
    const Option<string>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::agent::Call::KILL_NESTED_CONTAINER, call.type());
  CHECK(call.has_kill_nested_container());

  const ContainerID& containerId =
    call.kill_nested_container().container_id();

  Future<bool> destroy = slave->containerizer->destroy(containerId);

  return destroy
    .then([containerId](bool found) -> Response {
      if (!found) {
        return NotFound("Container '" + stringify(containerId) + "'"
                        " cannot be found (or is already killed)");
      }
      return OK();
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
