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

#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/array.hpp>

#include <mesos/type_utils.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <process/help.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/base64.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/representation.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "internal/devolve.hpp"

#include "logging/logging.hpp"

#include "master/master.hpp"
#include "master/validation.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

using process::Clock;
using process::DESCRIPTION;
using process::Future;
using process::HELP;
using process::TLDR;
using process::USAGE;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Forbidden;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotFound;
using process::http::NotImplemented;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::TemporaryRedirect;
using process::http::Unauthorized;
using process::http::UnsupportedMediaType;

using process::metrics::internal::MetricsProcess;

using std::map;
using std::string;
using std::vector;


namespace mesos {

void json(
    JSON::StringWriter* writer, const FrameworkInfo::Capability& capability)
{
  writer->append(FrameworkInfo::Capability::Type_Name(capability.type()));
}


void json(JSON::ObjectWriter* writer, const Offer& offer)
{
  writer->field("id", offer.id().value());
  writer->field("framework_id", offer.framework_id().value());
  writer->field("slave_id", offer.slave_id().value());
  writer->field("resources", Resources(offer.resources()));
}

namespace internal {
namespace master {

// Pull in model overrides from common.
using mesos::internal::model;

// Pull in definitions from process.
using process::http::Response;
using process::http::Request;


// The summary representation of `T` to support the `/state-summary` endpoint.
// e.g., `Summary<Slave>`.
template <typename T>
struct Summary : Representation<T>
{
  using Representation<T>::Representation;
};


// The full representation of `T` to support the `/state` endpoint.
// e.g., `Full<Slave>`.
template <typename T>
struct Full : Representation<T>
{
  using Representation<T>::Representation;
};


void json(JSON::ObjectWriter* writer, const Summary<Slave>& summary)
{
  const Slave& slave = summary;

  writer->field("id", slave.id.value());
  writer->field("pid", string(slave.pid));
  writer->field("hostname", slave.info.hostname());
  writer->field("registered_time", slave.registeredTime.secs());

  if (slave.reregisteredTime.isSome()) {
    writer->field("reregistered_time", slave.reregisteredTime.get().secs());
  }

  const Resources& totalResources = slave.totalResources;
  writer->field("resources", totalResources);
  writer->field("used_resources", Resources::sum(slave.usedResources));
  writer->field("offered_resources", slave.offeredResources);
  writer->field("reserved_resources", totalResources.reserved());
  writer->field("unreserved_resources", totalResources.unreserved());

  writer->field("attributes", Attributes(slave.info.attributes()));
  writer->field("active", slave.active);
}


void json(JSON::ObjectWriter* writer, const Full<Slave>& full)
{
  const Slave& slave = full;

  json(writer, Summary<Slave>(slave));
}


void json(JSON::ObjectWriter* writer, const Summary<Framework>& summary)
{
  const Framework& framework = summary;

  writer->field("id", framework.id().value());
  writer->field("name", framework.info.name());

  // Omit pid for http frameworks.
  if (framework.pid.isSome()) {
    writer->field("pid", string(framework.pid.get()));
  }

  // TODO(bmahler): Use these in the webui.
  writer->field("used_resources", framework.totalUsedResources);
  writer->field("offered_resources", framework.totalOfferedResources);
  writer->field("capabilities", framework.info.capabilities());
  writer->field("hostname", framework.info.hostname());
  writer->field("webui_url", framework.info.webui_url());
}


void json(JSON::ObjectWriter* writer, const Full<Framework>& full)
{
  const Framework& framework = full;

  json(writer, Summary<Framework>(framework));

  // Add additional fields to those generated by 'summarize'.
  writer->field("user", framework.info.user());
  writer->field("failover_timeout", framework.info.failover_timeout());
  writer->field("checkpoint", framework.info.checkpoint());
  writer->field("role", framework.info.role());
  writer->field("registered_time", framework.registeredTime.secs());
  writer->field("unregistered_time", framework.unregisteredTime.secs());

  // TODO(bmahler): Consider deprecating this in favor of the split
  // used and offered resources added in 'summarize'.
  writer->field(
      "resources",
      framework.totalUsedResources + framework.totalOfferedResources);

  // TODO(benh): Consider making reregisteredTime an Option.
  if (framework.registeredTime != framework.reregisteredTime) {
    writer->field("reregistered_time", framework.reregisteredTime.secs());
  }

  // Model all of the tasks associated with a framework.
  writer->field("tasks", [&framework](JSON::ArrayWriter* writer) {
    foreachvalue (const TaskInfo& taskInfo, framework.pendingTasks) {
      writer->element([&framework, &taskInfo](JSON::ObjectWriter* writer) {
        writer->field("id", taskInfo.task_id().value());
        writer->field("name", taskInfo.name());
        writer->field("framework_id", framework.id().value());

        writer->field("executor_id", taskInfo.executor().executor_id().value());

        writer->field("slave_id", taskInfo.slave_id().value());
        writer->field("state", TaskState_Name(TASK_STAGING));
        writer->field("resources", Resources(taskInfo.resources()));
        writer->field("statuses", std::initializer_list<TaskStatus>{});

        if (taskInfo.has_labels()) {
          writer->field("labels", taskInfo.labels());
        }

        if (taskInfo.has_discovery()) {
          writer->field("discovery", taskInfo.discovery());
        }
      });
    }

    foreachvalue (Task* task, framework.tasks) {
      writer->element(*task);
    }
  });

  writer->field("completed_tasks", [&framework](JSON::ArrayWriter* writer) {
    foreach (const std::shared_ptr<Task>& task, framework.completedTasks) {
      writer->element(*task);
    }
  });

  // Model all of the offers associated with a framework.
  writer->field("offers", [&framework](JSON::ArrayWriter* writer) {
    foreach (Offer* offer, framework.offers) {
      writer->element(*offer);
    }
  });

  // Model all of the executors of a framework.
  writer->field("executors", [&framework](JSON::ArrayWriter* writer) {
    foreachpair (
        const SlaveID& slaveId, const auto& executorsMap, framework.executors) {
      foreachvalue (const ExecutorInfo& executor, executorsMap) {
        writer->element([&executor, &slaveId](JSON::ObjectWriter* writer) {
          json(writer, executor);
          writer->field("slave_id", slaveId.value());
        });
      }
    }
  });

  // Model all of the labels associated with a framework.
  if (framework.info.has_labels()) {
    writer->field("labels", framework.info.labels());
  }
}


// TODO(bmahler): Kill these in favor of automatic Proto->JSON Conversion (when
// it becomes available).


// Returns a JSON object modeled on an Offer.
JSON::Object model(const Offer& offer)
{
  JSON::Object object;
  object.values["id"] = offer.id().value();
  object.values["framework_id"] = offer.framework_id().value();
  object.values["slave_id"] = offer.slave_id().value();
  object.values["resources"] = model(offer.resources());
  return object;
}


// Returns a JSON object summarizing some important fields in a
// Framework.
JSON::Object summarize(const Framework& framework)
{
  JSON::Object object;
  object.values["id"] = framework.id().value();
  object.values["name"] = framework.info.name();

  // Omit pid for http frameworks.
  if (framework.pid.isSome()) {
    object.values["pid"] = string(framework.pid.get());
  }

  // TODO(bmahler): Use these in the webui.
  object.values["used_resources"] = model(framework.totalUsedResources);
  object.values["offered_resources"] = model(framework.totalOfferedResources);

  {
      JSON::Array array;
      array.values.reserve(framework.info.capabilities_size());
      foreach (const FrameworkInfo::Capability& capability,
               framework.info.capabilities()) {
        array.values.push_back(
              FrameworkInfo::Capability::Type_Name(capability.type()));
      }
      object.values["capabilities"] = std::move(array);
  }

  object.values["hostname"] = framework.info.hostname();
  object.values["webui_url"] = framework.info.webui_url();

  return object;
}


// Returns a JSON object modeled on a Framework.
JSON::Object model(const Framework& framework)
{
  JSON::Object object = summarize(framework);

  // Add additional fields to those generated by 'summarize'.
  object.values["user"] = framework.info.user();
  object.values["failover_timeout"] = framework.info.failover_timeout();
  object.values["checkpoint"] = framework.info.checkpoint();
  object.values["role"] = framework.info.role();
  object.values["registered_time"] = framework.registeredTime.secs();
  object.values["unregistered_time"] = framework.unregisteredTime.secs();

  // TODO(bmahler): Consider deprecating this in favor of the split
  // used and offered resources added in 'summarize'.
  object.values["resources"] =
    model(framework.totalUsedResources + framework.totalOfferedResources);

  // TODO(benh): Consider making reregisteredTime an Option.
  if (framework.registeredTime != framework.reregisteredTime) {
    object.values["reregistered_time"] = framework.reregisteredTime.secs();
  }

  // Model all of the tasks associated with a framework.
  {
    JSON::Array array;
    array.values.reserve(
        framework.pendingTasks.size() + framework.tasks.size()); // MESOS-2353.

    foreachvalue (const TaskInfo& task, framework.pendingTasks) {
      vector<TaskStatus> statuses;
      array.values.push_back(
          model(task, framework.id(), TASK_STAGING, statuses));
    }

    foreachvalue (Task* task, framework.tasks) {
      array.values.push_back(model(*task));
    }

    object.values["tasks"] = std::move(array);
  }

  // Model all of the completed tasks of a framework.
  {
    JSON::Array array;
    array.values.reserve(framework.completedTasks.size()); // MESOS-2353.

    foreach (const std::shared_ptr<Task>& task, framework.completedTasks) {
      array.values.push_back(model(*task));
    }

    object.values["completed_tasks"] = std::move(array);
  }

  // Model all of the offers associated with a framework.
  {
    JSON::Array array;
    array.values.reserve(framework.offers.size()); // MESOS-2353.

    foreach (Offer* offer, framework.offers) {
      array.values.push_back(model(*offer));
    }

    object.values["offers"] = std::move(array);
  }

  // Model all of the executors of a framework.
  {
    JSON::Array executors;
    int executorSize = 0;
    foreachvalue (const auto& executorsMap,
                  framework.executors) {
      executorSize += executorsMap.size();
    }
    executors.values.reserve(executorSize); // MESOS-2353
    foreachpair (const SlaveID& slaveId,
                 const auto& executorsMap,
                 framework.executors) {
      foreachvalue (const ExecutorInfo& executor, executorsMap) {
        JSON::Object executorJson = model(executor);
        executorJson.values["slave_id"] = slaveId.value();
        executors.values.push_back(executorJson);
      }
    }

    object.values["executors"] = std::move(executors);
  }

  // Model all of the labels associated with a framework.
  if (framework.info.has_labels()) {
    JSON::Array array;
    const mesos::Labels labels = framework.info.labels();
    array.values.reserve(labels.labels_size());

    foreach (const Label& label, labels.labels()) {
      array.values.push_back(JSON::Protobuf(label));
    }
    object.values["labels"] = std::move(array);
  }

  return object;
}


// Returns a JSON object summarizing some important fields in a Slave.
JSON::Object summarize(const Slave& slave)
{
  JSON::Object object;
  object.values["id"] = slave.id.value();
  object.values["pid"] = string(slave.pid);
  object.values["hostname"] = slave.info.hostname();
  object.values["registered_time"] = slave.registeredTime.secs();

  if (slave.reregisteredTime.isSome()) {
    object.values["reregistered_time"] = slave.reregisteredTime.get().secs();
  }

  const Resources& totalResources = slave.totalResources;
  object.values["resources"] = model(totalResources);
  object.values["used_resources"] = model(Resources::sum(slave.usedResources));
  object.values["offered_resources"] = model(slave.offeredResources);
  object.values["reserved_resources"] = model(totalResources.reserved());
  object.values["unreserved_resources"] = model(totalResources.unreserved());

  object.values["attributes"] = model(slave.info.attributes());
  object.values["active"] = slave.active;
  return object;
}


// Returns a JSON object modeled after a Slave.
// For now there are no additional fields being added to those
// generated by 'summarize'.
JSON::Object model(const Slave& slave)
{
  return summarize(slave);
}


// Returns a JSON object modeled after a Role.
JSON::Object model(const Role& role)
{
  JSON::Object object;
  object.values["name"] = role.info.name();
  object.values["weight"] = role.info.weight();
  object.values["resources"] = model(role.resources());

  {
    JSON::Array array;

    foreachkey (const FrameworkID& frameworkId, role.frameworks) {
      array.values.push_back(frameworkId.value());
    }

    object.values["frameworks"] = std::move(array);
  }

  return object;
}


void Master::Http::log(const Request& request)
{
  Option<string> userAgent = request.headers.get("User-Agent");
  Option<string> forwardedFor = request.headers.get("X-Forwarded-For");

  LOG(INFO) << "HTTP " << request.method << " for " << request.path
            << " from " << request.client
            << (userAgent.isSome()
                ? " with User-Agent='" + userAgent.get() + "'"
                : "")
            << (forwardedFor.isSome()
                ? " with X-Forwarded-For='" + forwardedFor.get() + "'"
                : "");
}


// TODO(ijimenez): Add some information or pointers to help
// users understand the HTTP Event/Call API.
const string Master::Http::SCHEDULER_HELP = HELP(
    TLDR(
        "Endpoint for schedulers to make Calls against the master."),
    USAGE(
        "/api/v1/scheduler"),
    DESCRIPTION(
        "Returns 202 Accepted iff the request is accepted."));


Future<Response> Master::Http::scheduler(const Request& request) const
{
  // TODO(vinod): Add metrics for rejected requests.

  // TODO(vinod): Add support for rate limiting.

  if (!master->elected()) {
    // Note that this could happen if the scheduler realizes this is the
    // leading master before master itself realizes it (e.g., ZK watch delay).
    return ServiceUnavailable("Not the leading master");
  }

  CHECK_SOME(master->recovered);

  if (!master->recovered.get().isReady()) {
    return ServiceUnavailable("Master has not finished recovery");
  }

  if (master->flags.authenticate_frameworks) {
    return Unauthorized(
        "Mesos master",
        "HTTP schedulers are not supported when authentication is required");
  }

  if (request.method != "POST") {
    return MethodNotAllowed(
        "Expecting a 'POST' request, received '" + request.method + "'");
  }

  v1::scheduler::Call v1Call;

  // TODO(anand): Content type values are case-insensitive.
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

    Try<v1::scheduler::Call> parse =
      ::protobuf::parse<v1::scheduler::Call>(value.get());

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

  scheduler::Call call = devolve(v1Call);

  Option<Error> error = validation::scheduler::call::validate(call);

  if (error.isSome()) {
    return BadRequest("Failed to validate Scheduler::Call: " +
                      error.get().message);
  }

  if (call.type() == scheduler::Call::SUBSCRIBE) {
    // We default to JSON since an empty 'Accept' header
    // results in all media types considered acceptable.
    ContentType responseContentType;

    if (request.acceptsMediaType(APPLICATION_JSON)) {
      responseContentType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      responseContentType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }

    Pipe pipe;
    OK ok;
    ok.headers["Content-Type"] = stringify(responseContentType);

    ok.type = Response::PIPE;
    ok.reader = pipe.reader();

    HttpConnection http {pipe.writer(), responseContentType};
    master->subscribe(http, call.subscribe());

    return ok;
  }

  // We consolidate the framework lookup logic here because it is
  // common for all the call handlers.
  Framework* framework = master->getFramework(call.framework_id());

  if (framework == NULL) {
    return BadRequest("Framework cannot be found");
  }

  if (!framework->connected) {
    return Forbidden("Framework is not subscribed");
  }

  switch (call.type()) {
    case scheduler::Call::TEARDOWN:
      master->removeFramework(framework);
      return Accepted();

    case scheduler::Call::ACCEPT:
      master->accept(framework, call.accept());
      return Accepted();

    case scheduler::Call::DECLINE:
      master->decline(framework, call.decline());
      return Accepted();

    case scheduler::Call::REVIVE:
      master->revive(framework);
      return Accepted();

    case scheduler::Call::KILL:
      master->kill(framework, call.kill());
      return Accepted();

    case scheduler::Call::SHUTDOWN:
      master->shutdown(framework, call.shutdown());
      return Accepted();

    case scheduler::Call::ACKNOWLEDGE:
      master->acknowledge(framework, call.acknowledge());
      return Accepted();

    case scheduler::Call::RECONCILE:
      master->reconcile(framework, call.reconcile());
      return Accepted();

    case scheduler::Call::MESSAGE:
      master->message(framework, call.message());
      return Accepted();

    case scheduler::Call::REQUEST:
      master->request(framework, call.request());
      return Accepted();

    default:
      // Should be caught during call validation above.
      LOG(FATAL) << "Unexpected " << call.type() << " call";
  }

  return NotImplemented();
}


const string Master::Http::HEALTH_HELP = HELP(
    TLDR(
        "Health check of the Master."),
    USAGE(
        "/master/health"),
    DESCRIPTION(
        "Returns 200 OK iff the Master is healthy.",
        "Delayed responses are also indicative of poor health."));


Future<Response> Master::Http::health(const Request& request) const
{
  return OK();
}

const static string HOSTS_KEY = "hosts";
const static string LEVEL_KEY = "level";
const static string MONITOR_KEY = "monitor";

const string Master::Http::OBSERVE_HELP = HELP(
    TLDR(
        "Observe a monitor health state for host(s)."),
    USAGE(
        "/master/observe"),
    DESCRIPTION(
        "This endpoint receives information indicating host(s) ",
        "health."
        "",
        "The following fields should be supplied in a POST:",
        "1. " + MONITOR_KEY + " - name of the monitor that is being reported",
        "2. " + HOSTS_KEY + " - comma separated list of hosts",
        "3. " + LEVEL_KEY + " - OK for healthy, anything else for unhealthy"));


Try<string> getFormValue(
    const string& key,
    const hashmap<string, string>& values)
{
  Option<string> value = values.get(key);

  if (value.isNone()) {
    return Error("Missing value for '" + key + "'.");
  }

  // HTTP decode the value.
  Try<string> decodedValue = process::http::decode(value.get());
  if (decodedValue.isError()) {
    return decodedValue;
  }

  // Treat empty string as an error.
  if (decodedValue.isSome() && decodedValue.get().empty()) {
    return Error("Empty string for '" + key + "'.");
  }

  return decodedValue.get();
}


Future<Response> Master::Http::observe(const Request& request) const
{
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  hashmap<string, string> values = decode.get();

  // Build up a JSON object of the values we recieved and send them back
  // down the wire as JSON for validation / confirmation.
  JSON::Object response;

  // TODO(ccarson):  As soon as RepairCoordinator is introduced it will
  // consume these values. We should revisit if we still want to send the
  // JSON down the wire at that point.

  // Add 'monitor'.
  Try<string> monitor = getFormValue(MONITOR_KEY, values);
  if (monitor.isError()) {
    return BadRequest(monitor.error());
  }
  response.values[MONITOR_KEY] = monitor.get();

  // Add 'hosts'.
  Try<string> hostsString = getFormValue(HOSTS_KEY, values);
  if (hostsString.isError()) {
    return BadRequest(hostsString.error());
  }

  vector<string> hosts = strings::split(hostsString.get(), ",");
  JSON::Array hostArray;
  hostArray.values.assign(hosts.begin(), hosts.end());

  response.values[HOSTS_KEY] = hostArray;

  // Add 'isHealthy'.
  Try<string> level = getFormValue(LEVEL_KEY, values);
  if (level.isError()) {
    return BadRequest(level.error());
  }

  bool isHealthy = strings::upper(level.get()) == "OK";

  response.values["isHealthy"] = isHealthy;

  return OK(response);
}


const string Master::Http::REDIRECT_HELP = HELP(
    TLDR(
        "Redirects to the leading Master."),
    USAGE(
        "/master/redirect"),
    DESCRIPTION(
        "This returns a 307 Temporary Redirect to the leading Master.",
        "If no Master is leading (according to this Master), then the",
        "Master will redirect to itself.",
        "",
        "**NOTES:**",
        "1. This is the recommended way to bookmark the WebUI when",
        "running multiple Masters.",
        "2. This is broken currently \"on the cloud\" (e.g. EC2) as",
        "this will attempt to redirect to the private IP address, unless",
        "advertise_ip points to an externally accessible IP"));


Future<Response> Master::Http::redirect(const Request& request) const
{
  // If there's no leader, redirect to this master's base url.
  MasterInfo info = master->leader.isSome()
    ? master->leader.get()
    : master->info_;

  // NOTE: Currently, 'info.ip()' stores ip in network order, which
  // should be fixed. See MESOS-1201 for details.
  Try<string> hostname = info.has_hostname()
    ? info.hostname()
    : net::getHostname(net::IP(ntohl(info.ip())));

  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  // NOTE: We can use a protocol-relative URL here in order to allow
  // the browser (or other HTTP client) to prefix with 'http:' or
  // 'https:' depending on the original request. See
  // https://tools.ietf.org/html/rfc7231#section-7.1.2 as well as
  // http://stackoverflow.com/questions/12436669/using-protocol-relative-uris-within-location-headers
  // which discusses this.
  return TemporaryRedirect(
      "//" + hostname.get() + ":" + stringify(info.port()));
}


const string Master::Http::SLAVES_HELP = HELP(
    TLDR(
        "Information about registered slaves."),
    USAGE(
        "/master/slaves"),
    DESCRIPTION(
        "This endpoint shows information about the slaves registered in",
        "this master formatted as a JSON object."));


Future<Response> Master::Http::slaves(const Request& request) const
{
  JSON::Object object;

  {
    JSON::Array array;
    array.values.reserve(master->slaves.registered.size()); // MESOS-2353.

    foreachvalue (const Slave* slave, master->slaves.registered) {
      array.values.push_back(model(*slave));
    }

    object.values["slaves"] = std::move(array);
  }


  return OK(object, request.query.get("jsonp"));
}


const string Master::Http::STATE_HELP = HELP(
    TLDR(
        "Information about state of master."),
    USAGE(
        "/master/state.json"),
    DESCRIPTION(
        "This endpoint shows information about the frameworks, tasks,",
        "executors and slaves running in the cluster as a JSON object."));


Future<Response> Master::Http::state(const Request& request) const
{
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
    writer->field("start_time", master->startTime.secs());

    if (master->electedTime.isSome()) {
      writer->field("elected_time", master->electedTime.get().secs());
    }

    writer->field("id", master->info().id());
    writer->field("pid", string(master->self()));
    writer->field("hostname", master->info().hostname());
    writer->field("activated_slaves", master->_slaves_active());
    writer->field("deactivated_slaves", master->_slaves_inactive());

    if (master->flags.cluster.isSome()) {
      writer->field("cluster", master->flags.cluster.get());
    }

    if (master->leader.isSome()) {
      writer->field("leader", master->leader.get().pid());
    }

    if (master->flags.log_dir.isSome()) {
      writer->field("log_dir", master->flags.log_dir.get());
    }

    if (master->flags.external_log_file.isSome()) {
      writer->field("external_log_file", master->flags.external_log_file.get());
    }

    writer->field("flags", [this](JSON::ObjectWriter* writer) {
      foreachpair (const string& name, const flags::Flag& flag, master->flags) {
        Option<string> value = flag.stringify(master->flags);
        if (value.isSome()) {
          writer->field(name, value.get());
        }
      }
    });

    // Model all of the slaves.
    writer->field("slaves", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Slave* slave, master->slaves.registered) {
        writer->element(Full<Slave>(*slave));
      }
    });

    // Model all of the frameworks.
    writer->field("frameworks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Framework* framework, master->frameworks.registered) {
        writer->element(Full<Framework>(*framework));
      }
    });

    // Model all of the completed frameworks.
    writer->field("completed_frameworks", [this](JSON::ArrayWriter* writer) {
      foreach (const std::shared_ptr<Framework>& framework,
               master->frameworks.completed) {
        writer->element(Full<Framework>(*framework));
      }
    });

    // Model all of the orphan tasks.
    writer->field("orphan_tasks", [this](JSON::ArrayWriter* writer) {
      // Find those orphan tasks.
      foreachvalue (const Slave* slave, master->slaves.registered) {
        typedef hashmap<TaskID, Task*> TaskMap;
        foreachvalue (const TaskMap& tasks, slave->tasks) {
          foreachvalue (const Task* task, tasks) {
            CHECK_NOTNULL(task);
            if (!master->frameworks.registered.contains(task->framework_id())) {
              writer->element(*task);
            }
          }
        }
      }
    });

    // Model all currently unregistered frameworks.
    // This could happen when the framework has yet to re-register
    // after master failover.
    writer->field("unregistered_frameworks", [this](JSON::ArrayWriter* writer) {
      // Find unregistered frameworks.
      foreachvalue (const Slave* slave, master->slaves.registered) {
        foreachkey (const FrameworkID& frameworkId, slave->tasks) {
          if (!master->frameworks.registered.contains(frameworkId)) {
            writer->element(frameworkId.value());
          }
        }
      }
    });
  };

  return OK(jsonify(state), request.query.get("jsonp"));
}


// This abstraction has no side-effects. It factors out computing the
// mapping from 'slaves' to 'frameworks' to answer the questions 'what
// frameworks are running on a given slave?' and 'what slaves are
// running the given framework?'.
class SlaveFrameworkMapping
{
public:
  SlaveFrameworkMapping(const hashmap<FrameworkID, Framework*>& frameworks)
  {
    foreachpair (const FrameworkID& frameworkId,
                 const Framework* framework,
                 frameworks) {
      foreachvalue (const TaskInfo& taskInfo, framework->pendingTasks) {
        frameworksToSlaves[frameworkId].insert(taskInfo.slave_id());
        slavesToFrameworks[taskInfo.slave_id()].insert(frameworkId);
      }

      foreachvalue (const Task* task, framework->tasks) {
        frameworksToSlaves[frameworkId].insert(task->slave_id());
        slavesToFrameworks[task->slave_id()].insert(frameworkId);
      }

      foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
        frameworksToSlaves[frameworkId].insert(task->slave_id());
        slavesToFrameworks[task->slave_id()].insert(frameworkId);
      }
    }
  }

  const hashset<FrameworkID>& frameworks(const SlaveID& slaveId) const
  {
    const auto iterator = slavesToFrameworks.find(slaveId);
    return iterator != slavesToFrameworks.end() ?
      iterator->second : hashset<FrameworkID>::EMPTY;
  }

  const hashset<SlaveID>& slaves(const FrameworkID& frameworkId) const
  {
    const auto iterator = frameworksToSlaves.find(frameworkId);
    return iterator != frameworksToSlaves.end() ?
      iterator->second : hashset<SlaveID>::EMPTY;
  }

private:
  hashmap<SlaveID, hashset<FrameworkID>> slavesToFrameworks;
  hashmap<FrameworkID, hashset<SlaveID>> frameworksToSlaves;
};


// This abstraction has no side-effects. It factors out the accounting
// for a 'TaskState' summary. We use this to summarize 'TaskState's
// for both frameworks as well as slaves.
struct TaskStateSummary
{
  // TODO(jmlvanre): Possibly clean this up as per MESOS-2694.
  const static TaskStateSummary EMPTY;

  TaskStateSummary()
    : staging(0),
      starting(0),
      running(0),
      finished(0),
      killed(0),
      failed(0),
      lost(0),
      error(0) {}

  // Account for the state of the given task.
  void count(const Task& task)
  {
    switch (task.state()) {
      case TASK_STAGING: { ++staging; break; }
      case TASK_STARTING: { ++starting; break; }
      case TASK_RUNNING: { ++running; break; }
      case TASK_FINISHED: { ++finished; break; }
      case TASK_KILLED: { ++killed; break; }
      case TASK_FAILED: { ++failed; break; }
      case TASK_LOST: { ++lost; break; }
      case TASK_ERROR: { ++error; break; }
      // No default case allows for a helpful compiler error if we
      // introduce a new state.
    }
  }

  size_t staging;
  size_t starting;
  size_t running;
  size_t finished;
  size_t killed;
  size_t failed;
  size_t lost;
  size_t error;
};


const TaskStateSummary TaskStateSummary::EMPTY;


// This abstraction has no side-effects. It factors out computing the
// 'TaskState' summaries for frameworks and slaves. This answers the
// questions 'How many tasks are in each state for a given framework?'
// and 'How many tasks are in each state for a given slave?'.
class TaskStateSummaries
{
public:
  TaskStateSummaries(const hashmap<FrameworkID, Framework*>& frameworks)
  {
    foreachpair (const FrameworkID& frameworkId,
                 const Framework* framework,
                 frameworks) {
      foreachvalue (const TaskInfo& taskInfo, framework->pendingTasks) {
        frameworkTaskSummaries[frameworkId].staging++;
        slaveTaskSummaries[taskInfo.slave_id()].staging++;
      }

      foreachvalue (const Task* task, framework->tasks) {
        frameworkTaskSummaries[frameworkId].count(*task);
        slaveTaskSummaries[task->slave_id()].count(*task);
      }

      foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
        frameworkTaskSummaries[frameworkId].count(*task);
        slaveTaskSummaries[task->slave_id()].count(*task);
      }
    }
  }

  const TaskStateSummary& framework(const FrameworkID& frameworkId) const
  {
    const auto iterator = frameworkTaskSummaries.find(frameworkId);
    return iterator != frameworkTaskSummaries.end() ?
      iterator->second : TaskStateSummary::EMPTY;
  }

  const TaskStateSummary& slave(const SlaveID& slaveId) const
  {
    const auto iterator = slaveTaskSummaries.find(slaveId);
    return iterator != slaveTaskSummaries.end() ?
      iterator->second : TaskStateSummary::EMPTY;
  }
private:
  hashmap<FrameworkID, TaskStateSummary> frameworkTaskSummaries;
  hashmap<SlaveID, TaskStateSummary> slaveTaskSummaries;
};


const string Master::Http::STATESUMMARY_HELP = HELP(
    TLDR(
        "Summary of state of all tasks and registered frameworks in cluster."),
    USAGE(
        "/master/state-summary"),
    DESCRIPTION(
        "This endpoint gives a summary of the state of all tasks and",
        "registered frameworks in the cluster as a JSON object."));


Future<Response> Master::Http::stateSummary(const Request& request) const
{
  auto stateSummary = [this](JSON::ObjectWriter* writer) {
    writer->field("hostname", master->info().hostname());

    if (master->flags.cluster.isSome()) {
      writer->field("cluster", master->flags.cluster.get());
    }

    // We use the tasks in the 'Frameworks' struct to compute summaries
    // for this endpoint. This is done 1) for consistency between the
    // 'slaves' and 'frameworks' subsections below 2) because we want to
    // provide summary information for frameworks that are currently
    // registered 3) the frameworks keep a circular buffer of completed
    // tasks that we can use to keep a limited view on the history of
    // recent completed / failed tasks.

    // Generate mappings from 'slave' to 'framework' and reverse.
    SlaveFrameworkMapping slaveFrameworkMapping(master->frameworks.registered);

    // Generate 'TaskState' summaries for all framework and slave ids.
    TaskStateSummaries taskStateSummaries(master->frameworks.registered);

    // Model all of the slaves.
    writer->field("slaves", [this,
                             &slaveFrameworkMapping,
                             &taskStateSummaries](JSON::ArrayWriter* writer) {
      foreachvalue (Slave* slave, master->slaves.registered) {
        writer->element([&slave,
                         &slaveFrameworkMapping,
                         &taskStateSummaries](JSON::ObjectWriter* writer) {
          json(writer, Summary<Slave>(*slave));

          // Add the 'TaskState' summary for this slave.
          const TaskStateSummary& summary = taskStateSummaries.slave(slave->id);

          writer->field("TASK_STAGING", summary.staging);
          writer->field("TASK_STARTING", summary.starting);
          writer->field("TASK_RUNNING", summary.running);
          writer->field("TASK_FINISHED", summary.finished);
          writer->field("TASK_KILLED", summary.killed);
          writer->field("TASK_FAILED", summary.failed);
          writer->field("TASK_LOST", summary.lost);
          writer->field("TASK_ERROR", summary.error);

          // Add the ids of all the frameworks running on this slave.
          const hashset<FrameworkID>& frameworks =
            slaveFrameworkMapping.frameworks(slave->id);

          writer->field("framework_ids",
                        [&frameworks](JSON::ArrayWriter* writer) {
            foreach (const FrameworkID& frameworkId, frameworks) {
              writer->element(frameworkId.value());
            }
          });
        });
      }
    });

    // Model all of the frameworks.
    writer->field("frameworks",
                  [this,
                   &slaveFrameworkMapping,
                   &taskStateSummaries](JSON::ArrayWriter* writer) {
      foreachpair (const FrameworkID& frameworkId,
                   Framework* framework,
                   master->frameworks.registered) {
        writer->element([&frameworkId,
                         &framework,
                         &slaveFrameworkMapping,
                         &taskStateSummaries](JSON::ObjectWriter* writer) {
          json(writer, Summary<Framework>(*framework));

          // Add the 'TaskState' summary for this framework.
          const TaskStateSummary& summary =
            taskStateSummaries.framework(frameworkId);

          writer->field("TASK_STAGING", summary.staging);
          writer->field("TASK_STARTING", summary.starting);
          writer->field("TASK_RUNNING", summary.running);
          writer->field("TASK_FINISHED", summary.finished);
          writer->field("TASK_KILLED", summary.killed);
          writer->field("TASK_FAILED", summary.failed);
          writer->field("TASK_LOST", summary.lost);
          writer->field("TASK_ERROR", summary.error);

          // Add the ids of all the slaves running this framework.
          const hashset<SlaveID>& slaves =
            slaveFrameworkMapping.slaves(frameworkId);

          writer->field("slave_ids", [&slaves](JSON::ArrayWriter* writer) {
            foreach (const SlaveID& slaveId, slaves) {
              writer->element(slaveId.value());
            }
          });
        });
      }
    });
  };

  return OK(jsonify(stateSummary), request.query.get("jsonp"));
}


const string Master::Http::ROLES_HELP = HELP(
    TLDR(
        "Information about roles that the master is configured with."),
    USAGE(
        "/master/roles.json"),
    DESCRIPTION(
        "This endpoint gives information about the roles that are assigned",
        "to frameworks and resources as a JSON object."));


Future<Response> Master::Http::roles(const Request& request) const
{
  JSON::Object object;

  // Model all of the roles.
  {
    JSON::Array array;
    foreachvalue (Role* role, master->roles) {
      array.values.push_back(model(*role));
    }

    object.values["roles"] = std::move(array);
  }

  return OK(object, request.query.get("jsonp"));
}


const string Master::Http::TEARDOWN_HELP = HELP(
    TLDR(
        "Tears down a running framework by shutting down all tasks/executors "
        "and removing the framework."),
    USAGE(
        "/master/teardown"),
    DESCRIPTION(
        "Please provide a \"frameworkId\" value designating the running "
        "framework to tear down.",
        "Returns 200 OK if the framework was correctly teared down."));


Future<Response> Master::Http::teardown(const Request& request) const
{
  if (request.method != "POST") {
    return BadRequest("Expecting POST");
  }

  // Parse the query string in the request body (since this is a POST)
  // in order to determine the framework ID to shutdown.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  hashmap<string, string> values = decode.get();

  if (values.get("frameworkId").isNone()) {
    return BadRequest("Missing 'frameworkId' query parameter");
  }

  FrameworkID id;
  id.set_value(values.get("frameworkId").get());

  Framework* framework = master->getFramework(id);

  if (framework == NULL) {
    return BadRequest("No framework found with specified ID");
  }

  Result<Credential> credential = authenticate(request);

  if (credential.isError()) {
    return Unauthorized("Mesos master", credential.error());
  }

  // Skip authorization if no ACLs were provided to the master.
  if (master->authorizer.isNone()) {
    return _teardown(id);
  }

  mesos::ACL::ShutdownFramework shutdown;

  if (credential.isSome()) {
    shutdown.mutable_principals()->add_values(credential.get().principal());
  } else {
    shutdown.mutable_principals()->set_type(ACL::Entity::ANY);
  }

  if (framework->info.has_principal()) {
    shutdown.mutable_framework_principals()->add_values(
        framework->info.principal());
  } else {
    shutdown.mutable_framework_principals()->set_type(ACL::Entity::ANY);
  }

  lambda::function<Future<Response>(bool)> _teardown =
    lambda::bind(&Master::Http::_teardown, this, id, lambda::_1);

  return master->authorizer.get()->authorize(shutdown)
    .then(defer(master->self(), _teardown));
}


Future<Response> Master::Http::_teardown(
    const FrameworkID& id,
    bool authorized) const
{
  if (!authorized) {
    return Unauthorized("Mesos master");
  }

  Framework* framework = master->getFramework(id);

  if (framework == NULL) {
    return BadRequest("No framework found with ID " + stringify(id));
  }

  // TODO(ijimenez): Do 'removeFramework' asynchronously.
  master->removeFramework(framework);

  return OK();
}


const string Master::Http::TASKS_HELP = HELP(
    TLDR(
      "Lists tasks from all active frameworks."),
    USAGE(
      "/master/tasks.json"),
    DESCRIPTION(
      "Lists known tasks.",
      "",
      "Query parameters:",
      "",
      ">        limit=VALUE          Maximum number of tasks returned "
      "(default is " + stringify(TASK_LIMIT) + ").",
      ">        offset=VALUE         Starts task list at offset.",
      ">        order=(asc|desc)     Ascending or descending sort order "
      "(default is descending)."
      ""));


struct TaskComparator
{
  static bool ascending(const Task* lhs, const Task* rhs)
  {
    size_t lhsSize = lhs->statuses().size();
    size_t rhsSize = rhs->statuses().size();

    if ((lhsSize == 0) && (rhsSize == 0)) {
      return false;
    }

    if (lhsSize == 0) {
      return true;
    }

    if (rhsSize == 0) {
      return false;
    }

    return (lhs->statuses(0).timestamp() < rhs->statuses(0).timestamp());
  }

  static bool descending(const Task* lhs, const Task* rhs)
  {
    size_t lhsSize = lhs->statuses().size();
    size_t rhsSize = rhs->statuses().size();

    if ((lhsSize == 0) && (rhsSize == 0)) {
      return false;
    }

    if (rhsSize == 0) {
      return true;
    }

    if (lhsSize == 0) {
      return false;
    }

    return (lhs->statuses(0).timestamp() > rhs->statuses(0).timestamp());
  }
};


Future<Response> Master::Http::tasks(const Request& request) const
{
  // Get list options (limit and offset).
  Result<int> result = numify<int>(request.query.get("limit"));
  size_t limit = result.isSome() ? result.get() : TASK_LIMIT;

  result = numify<int>(request.query.get("offset"));
  size_t offset = result.isSome() ? result.get() : 0;

  // TODO(nnielsen): Currently, formatting errors in offset and/or limit
  // will silently be ignored. This could be reported to the user instead.

  // Construct framework list with both active and completed framwworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, master->frameworks.registered) {
    frameworks.push_back(framework);
  }
  foreach (const std::shared_ptr<Framework>& framework,
           master->frameworks.completed) {
    frameworks.push_back(framework.get());
  }

  // Construct task list with both running and finished tasks.
  vector<const Task*> tasks;
  foreach (const Framework* framework, frameworks) {
    foreachvalue (Task* task, framework->tasks) {
      CHECK_NOTNULL(task);
      tasks.push_back(task);
    }
    foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
      tasks.push_back(task.get());
    }
  }

  // Sort tasks by task status timestamp. Default order is descending.
  // The earliest timestamp is chosen for comparison when multiple are present.
  Option<string> order = request.query.get("order");
  if (order.isSome() && (order.get() == "asc")) {
    sort(tasks.begin(), tasks.end(), TaskComparator::ascending);
  } else {
    sort(tasks.begin(), tasks.end(), TaskComparator::descending);
  }

  JSON::Object object;

  {
    JSON::Array array;
    size_t end = std::min(offset + limit, tasks.size());
    for (size_t i = offset; i < end; i++) {
      const Task* task = tasks[i];
      array.values.push_back(model(*task));
    }

    object.values["tasks"] = std::move(array);
  }

  return OK(object, request.query.get("jsonp"));
}


Result<Credential> Master::Http::authenticate(const Request& request) const
{
  // By default, assume everyone is authenticated if no credentials
  // were provided.
  if (master->credentials.isNone()) {
    return None();
  }

  Option<string> authorization = request.headers.get("Authorization");

  if (authorization.isNone()) {
    return Error("Missing 'Authorization' request header");
  }

  Try<string> decode =
    base64::decode(strings::split(authorization.get(), " ", 2)[1]);

  if (decode.isError()) {
    return Error("Failed to decode 'Authorization' header: " + decode.error());
  }

  vector<string> pairs = strings::split(decode.get(), ":", 2);

  if (pairs.size() != 2) {
    return Error("Malformed 'Authorization' request header");
  }

  const string& username = pairs[0];
  const string& password = pairs[1];

  foreach (const Credential& credential,
          master->credentials.get().credentials()) {
    if (credential.principal() == username &&
        credential.secret() == password) {
      return credential;
    }
  }

  return Error("Could not authenticate '" + username + "'");
}


} // namespace master {
} // namespace internal {
} // namespace mesos {
