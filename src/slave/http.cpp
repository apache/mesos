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

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <process/collect.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
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

#include "common/build.hpp"
#include "common/http.hpp"

#include "internal/devolve.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/slave.hpp"
#include "slave/validation.hpp"

using process::AUTHENTICATION;
using process::AUTHORIZATION;
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

using std::list;
using std::string;
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


static void json(JSON::ObjectWriter* writer, const Executor& executor)
{
  writer->field("id", executor.id.value());
  writer->field("name", executor.info.name());
  writer->field("source", executor.info.source());
  writer->field("container", executor.containerId.value());
  writer->field("directory", executor.directory);
  writer->field("resources", executor.resources);

  if (executor.info.has_labels()) {
    writer->field("labels", executor.info.labels());
  }

  writer->field("tasks", [&executor](JSON::ArrayWriter* writer) {
    foreach (Task* task, executor.launchedTasks.values()) {
      writer->element(*task);
    }
  });

  writer->field("queued_tasks", [&executor](JSON::ArrayWriter* writer) {
    foreach (const TaskInfo& task, executor.queuedTasks.values()) {
      writer->element(task);
    }
  });

  writer->field("completed_tasks", [&executor](JSON::ArrayWriter* writer) {
    foreach (const std::shared_ptr<Task>& task, executor.completedTasks) {
      writer->element(*task);
    }

    // NOTE: We add 'terminatedTasks' to 'completed_tasks' for
    // simplicity.
    // TODO(vinod): Use foreachvalue instead once LinkedHashmap
    // supports it.
    foreach (Task* task, executor.terminatedTasks.values()) {
      writer->element(*task);
    }
  });
}


static void json(JSON::ObjectWriter* writer, const Framework& framework)
{
  writer->field("id", framework.id().value());
  writer->field("name", framework.info.name());
  writer->field("user", framework.info.user());
  writer->field("failover_timeout", framework.info.failover_timeout());
  writer->field("checkpoint", framework.info.checkpoint());
  writer->field("role", framework.info.role());
  writer->field("hostname", framework.info.hostname());

  writer->field("executors", [&framework](JSON::ArrayWriter* writer) {
    foreachvalue (Executor* executor, framework.executors) {
      writer->element(*executor);
    }
  });

  writer->field("completed_executors", [&framework](JSON::ArrayWriter* writer) {
    foreach (const Owned<Executor>& executor, framework.completedExecutors) {
      writer->element(*executor);
    }
  });
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
        "The request principal should be authorized to query this endpoint.",
        "See the authorization documentation for details."));
}


Future<Response> Slave::Http::flags(
    const Request& request,
    const Option<string>& principal) const
{
  const Flags slaveFlags = slave->flags;

  return authorizeEndpoint(request, principal)
    .then(defer(
        slave->self(),
        [request, slaveFlags](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return _flags(request, slaveFlags);
        }));
}


Future<Response> Slave::Http::_flags(
  const Request& request,
  const Flags& slaveFlags)
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachpair (const string& name, const flags::Flag& flag, slaveFlags) {
      Option<string> value = flag.stringify(slaveFlags);
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


string Slave::Http::STATE_HELP() {
  return HELP(
    TLDR(
        "Information about state of the Agent."),
    DESCRIPTION(
        "This endpoint shows information about the frameworks, executors",
        "and the agent's master as a JSON object.",
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
    AUTHENTICATION(true));
}


Future<Response> Slave::Http::state(
    const Request& request,
    const Option<string>& /* principal */) const
{
  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  auto state = [this](JSON::ObjectWriter* writer) {
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

    writer->field("resources", Resources(slave->info.resources()));
    writer->field("attributes", Attributes(slave->info.attributes()));

    if (slave->master.isSome()) {
      Try<string> hostname = net::getHostname(slave->master.get().address.ip);
      if (hostname.isSome()) {
        writer->field("master_hostname", hostname.get());
      }
    }

    if (slave->flags.log_dir.isSome()) {
      writer->field("log_dir", slave->flags.log_dir.get());
    }

    if (slave->flags.external_log_file.isSome()) {
      writer->field("external_log_file", slave->flags.external_log_file.get());
    }

    writer->field("frameworks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Framework* framework, slave->frameworks) {
        writer->element(*framework);
      }
    });

    // Model all of the completed frameworks.
    writer->field("completed_frameworks", [this](JSON::ArrayWriter* writer) {
      foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
        writer->element(*framework);
      }
    });

    writer->field("flags", [this](JSON::ObjectWriter* writer) {
      foreachpair (const string& name, const flags::Flag& flag, slave->flags) {
        Option<string> value = flag.stringify(slave->flags);
        if (value.isSome()) {
          writer->field(name, value.get());
        }
      }
    });
  };

  return OK(jsonify(state), request.url.query.get("jsonp"));
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
  const PID<Slave> pid = slave->self();
  Shared<RateLimiter> limiter = statisticsLimiter;

  return authorizeEndpoint(request, principal)
    .then(defer(
        pid,
        [pid, limiter, request](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return limiter->acquire()
            .then(defer(pid, &Slave::usage))
            .then(defer(pid, [request](const ResourceUsage& usage) {
              return _statistics(usage, request);
            }));
        }))
    .repair([](const Future<Response>& future) {
      LOG(WARNING) << "Could not collect statistics: "
                   << (future.isFailed() ? future.failure() : "discarded");

      return InternalServerError();
    });
}


Response Slave::Http::_statistics(
    const ResourceUsage& usage,
    const Request& request)
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
          "```"));
}


Future<Response> Slave::Http::containers(const Request& request) const
{
  Owned<list<JSON::Object>> metadata(new list<JSON::Object>());
  list<Future<ContainerStatus>> statusFutures;
  list<Future<ResourceStatistics>> statsFutures;

  foreachvalue (const Framework* framework, slave->frameworks) {
    foreachvalue (const Executor* executor, framework->executors) {
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
      [metadata, request](const tuple<
          Future<list<Future<ContainerStatus>>>,
          Future<list<Future<ResourceStatistics>>>>& t)
          -> Future<Response> {
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

        return process::http::OK(result, request.url.query.get("jsonp"));
      })
      .repair([](const Future<Response>& future) {
        LOG(WARNING) << "Could not collect container status and statistics: "
                     << (future.isFailed() ? future.failure() : "discarded");

        return process::http::InternalServerError();
      });
}


Future<bool> Slave::Http::authorizeEndpoint(
    const Request& httpRequest,
    const Option<string>& principal) const
{
  if (slave->authorizer.isNone()) {
    return true;
  }

  // Paths are of the form "/slave(n)/endpoint". We're only interested
  // in the part after "/slave(n)" and tokenize the path accordingly.
  const vector<string> pathComponents =
    strings::tokenize(httpRequest.url.path, "/", 2);

  if (pathComponents.size() != 2u ||
      pathComponents[0] != slave->self().id) {
    return Failure("Unexpected path '" + httpRequest.url.path + "'");
  }
  const string endpoint = "/" + pathComponents[1];

  authorization::Request authorizationRequest;

  // TODO(nfnt): Add an additional case when POST requests need to be
  // authorized separately from GET requests.
  if (httpRequest.method == "GET") {
    authorizationRequest.set_action(authorization::GET_ENDPOINT_WITH_PATH);
  } else {
    return Failure("Unexpected request method '" + httpRequest.method + "'");
  }

  if (principal.isSome()) {
    authorizationRequest.mutable_subject()->set_value(principal.get());
  }

  authorizationRequest.mutable_object()->set_value(endpoint);

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? principal.get() : "ANY")
            << "' to " <<  httpRequest.method
            << " the '" << endpoint << "' endpoint";

  return slave->authorizer.get()->authorized(authorizationRequest);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
