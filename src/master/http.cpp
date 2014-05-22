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
#include <sstream>
#include <string>
#include <vector>

#include <boost/array.hpp>

#include <process/help.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "master/master.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

using process::Clock;
using process::DESCRIPTION;
using process::Future;
using process::HELP;
using process::TLDR;
using process::USAGE;

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::TemporaryRedirect;

using process::metrics::internal::MetricsProcess;

using std::map;
using std::string;
using std::vector;


namespace mesos {
namespace internal {
namespace master {

// Pull in model overrides from common.
using mesos::internal::model;

// Pull in definitions from process.
using process::http::Response;
using process::http::Request;


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


// Returns a JSON object modeled on a Framework.
JSON::Object model(const Framework& framework)
{
  JSON::Object object;
  object.values["id"] = framework.id.value();
  object.values["name"] = framework.info.name();
  object.values["user"] = framework.info.user();
  object.values["failover_timeout"] = framework.info.failover_timeout();
  object.values["checkpoint"] = framework.info.checkpoint();
  object.values["role"] = framework.info.role();
  object.values["registered_time"] = framework.registeredTime.secs();
  object.values["unregistered_time"] = framework.unregisteredTime.secs();
  object.values["active"] = framework.active;
  object.values["resources"] = model(framework.resources);
  object.values["hostname"] = framework.info.hostname();

  // TODO(benh): Consider making reregisteredTime an Option.
  if (framework.registeredTime != framework.reregisteredTime) {
    object.values["reregistered_time"] = framework.reregisteredTime.secs();
  }

  // Model all of the tasks associated with a framework.
  {
    JSON::Array array;
    foreachvalue (Task* task, framework.tasks) {
      array.values.push_back(model(*task));
    }

    object.values["tasks"] = array;
  }

  // Model all of the completed tasks of a framework.
  {
    JSON::Array array;
    foreach (const memory::shared_ptr<Task>& task, framework.completedTasks) {
      array.values.push_back(model(*task));
    }

    object.values["completed_tasks"] = array;
  }

  // Model all of the offers associated with a framework.
  {
    JSON::Array array;
    foreach (Offer* offer, framework.offers) {
      array.values.push_back(model(*offer));
    }

    object.values["offers"] = array;
  }

  return object;
}


// Returns a JSON object modeled after a Slave.
JSON::Object model(const Slave& slave)
{
  JSON::Object object;
  object.values["id"] = slave.id.value();
  object.values["pid"] = string(slave.pid);
  object.values["hostname"] = slave.info.hostname();
  object.values["registered_time"] = slave.registeredTime.secs();

  if (slave.reregisteredTime.isSome()) {
    object.values["reregistered_time"] = slave.reregisteredTime.get().secs();
  }

  object.values["resources"] = model(slave.info.resources());
  object.values["attributes"] = model(slave.info.attributes());
  return object;
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

    object.values["frameworks"] = array;
  }

  return object;
}


const string Master::Http::HEALTH_HELP = HELP(
    TLDR(
        "Health check of the Master."),
    USAGE(
        "/master/health"),
    DESCRIPTION(
        "Returns 200 OK iff the Master is healthy.",
        "Delayed responses are also indicative of poor health."));


Future<Response> Master::Http::health(const Request& request)
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
        "2. " + HOSTS_KEY + " - comma seperated list of hosts",
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


Future<Response> Master::Http::observe(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  hashmap<string, string> values =
    process::http::query::parse(request.body);

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
        "this will attempt to redirect to the private IP address."));


Future<Response> Master::Http::redirect(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  // If there's no leader, redirect to this master's base url.
  MasterInfo info = master.leader.isSome() ? master.leader.get() : master.info_;

  Try<string> hostname =
    info.has_hostname() ? info.hostname() : net::getHostname(info.ip());

  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  return TemporaryRedirect(
      "http://" + hostname.get() + ":" + stringify(info.port()));
}


// Declaration of 'stats' continuation.
static Future<Response> _stats(
    const Request& request,
    JSON::Object object,
    const Response& response);


Future<Response> Master::Http::stats(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = (Clock::now() - master.startTime).secs();
  object.values["elected"] = master.elected() ? 1 : 0;
  object.values["total_schedulers"] = master.frameworks.activated.size();
  object.values["active_schedulers"] = master.getActiveFrameworks().size();
  object.values["activated_slaves"] = master.slaves.activated.size();
  object.values["deactivated_slaves"] = master.slaves.deactivated.size();
  object.values["outstanding_offers"] = master.offers.size();

  // NOTE: These are monotonically increasing counters.
  object.values["staged_tasks"] = master.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master.stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = master.stats.validStatusUpdates;
  object.values["invalid_status_updates"] = master.stats.invalidStatusUpdates;

  // Get a count of all active tasks in the cluster i.e., the tasks
  // that are launched (TASK_STAGING, TASK_STARTING, TASK_RUNNING) but
  // haven't reached terminal state yet.
  // NOTE: This is a gauge representing an instantaneous value.
  int active_tasks = 0;
  foreachvalue (Framework* framework, master.frameworks.activated) {
    active_tasks += framework->tasks.size();
  }
  object.values["active_tasks_gauge"] = active_tasks;

  // Get total and used (note, not offered) resources in order to
  // compute capacity of scalar resources.
  Resources totalResources;
  Resources usedResources;
  foreachvalue (Slave* slave, master.slaves.activated) {
    // Instead of accumulating all types of resources (which is
    // not necessary), we only accumulate scalar resources. This
    // helps us bypass a performance problem caused by range
    // additions (e.g. ports).
    foreach (const Resource& resource, slave->info.resources()) {
      if (resource.type() == Value::SCALAR) {
        totalResources += resource;
      }
    }
    foreach (const Resource& resource, slave->resourcesInUse) {
      if (resource.type() == Value::SCALAR) {
        usedResources += resource;
      }
    }
  }

  foreach (const Resource& resource, totalResources) {
    CHECK(resource.has_scalar());
    double total = resource.scalar().value();
    object.values[resource.name() + "_total"] = total;
    Option<Resource> option = usedResources.get(resource);
    CHECK(!option.isSome() || option.get().has_scalar());
    double used = option.isSome() ? option.get().scalar().value() : 0.0;
    object.values[resource.name() + "_used"] = used;
    double percent = used / total;
    object.values[resource.name() + "_percent"] = percent;
  }

  // Include metrics from libprocess metrics while we sunset this
  // endpoint in favor of libprocess metrics.
  // TODO(benh): Remove this after transitioning to libprocess metrics.
  return process::http::get(MetricsProcess::instance()->self(), "snapshot")
    .then(lambda::bind(&_stats, request, object, lambda::_1));
}


static Future<Response> _stats(
    const Request& request,
    JSON::Object object,
    const Response& response)
{
  if (response.status != process::http::statuses[200]) {
    return InternalServerError("Failed to get metrics: " + response.status);
  }

  Option<string> type = response.headers.get("Content-Type");

  if (type.isNone() || type.get() != "application/json") {
    return InternalServerError("Failed to get metrics: expecting JSON");
  }

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.body);

  if (parse.isError()) {
    return InternalServerError("Failed to parse metrics: " + parse.error());
  }

  // Now add all the values from metrics.
  // TODO(benh): Make sure we're not overwriting any values.
  object.values.insert(parse.get().values.begin(), parse.get().values.end());

  return OK(object, request.query.get("jsonp"));
}


Future<Response> Master::Http::state(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

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
  object.values["start_time"] = master.startTime.secs();

  if (master.electedTime.isSome()) {
    object.values["elected_time"] = master.electedTime.get().secs();
  }

  object.values["id"] = master.info().id();
  object.values["pid"] = string(master.self());
  object.values["hostname"] = master.info().hostname();
  object.values["activated_slaves"] = master.slaves.activated.size();
  object.values["deactivated_slaves"] = master.slaves.deactivated.size();
  object.values["staged_tasks"] = master.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master.stats.tasks[TASK_LOST];

  if (master.flags.cluster.isSome()) {
    object.values["cluster"] = master.flags.cluster.get();
  }

  if (master.leader.isSome()) {
    object.values["leader"] = master.leader.get().pid();
  }

  if (master.flags.log_dir.isSome()) {
    object.values["log_dir"] = master.flags.log_dir.get();
  }

  JSON::Object flags;
  foreachpair (const string& name, const flags::Flag& flag, master.flags) {
    Option<string> value = flag.stringify(master.flags);
    if (value.isSome()) {
      flags.values[name] = value.get();
    }
  }
  object.values["flags"] = flags;

  // Model all of the slaves.
  {
    JSON::Array array;
    foreachvalue (Slave* slave, master.slaves.activated) {
      array.values.push_back(model(*slave));
    }

    object.values["slaves"] = array;
  }

  // Model all of the frameworks.
  {
    JSON::Array array;
    foreachvalue (Framework* framework, master.frameworks.activated) {
      array.values.push_back(model(*framework));
    }

    object.values["frameworks"] = array;
  }

  // Model all of the completed frameworks.
  {
    JSON::Array array;

    foreach (const memory::shared_ptr<Framework>& framework,
             master.frameworks.completed) {
      array.values.push_back(model(*framework));
    }

    object.values["completed_frameworks"] = array;
  }

  return OK(object, request.query.get("jsonp"));
}


Future<Response> Master::Http::roles(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;

  // Model all of the roles.
  {
    JSON::Array array;
    foreachvalue (Role* role, master.roles) {
      array.values.push_back(model(*role));
    }

    object.values["roles"] = array;
  }

  return OK(object, request.query.get("jsonp"));
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


Future<Response> Master::Http::tasks(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  // Get list options (limit and offset).
  Result<int> result = numify<int>(request.query.get("limit"));
  size_t limit = result.isSome() ? result.get() : TASK_LIMIT;

  result = numify<int>(request.query.get("offset"));
  size_t offset = result.isSome() ? result.get() : 0;

  // TODO(nnielsen): Currently, formatting errors in offset and/or limit
  // will silently be ignored. This could be reported to the user instead.

  // Construct framework list with both active and completed framwworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, master.frameworks.activated) {
    frameworks.push_back(framework);
  }
  foreach (const memory::shared_ptr<Framework>& framework,
           master.frameworks.completed) {
    frameworks.push_back(framework.get());
  }

  // Construct task list with both running and finished tasks.
  vector<const Task*> tasks;
  foreach (const Framework* framework, frameworks) {
    foreachvalue (Task* task, framework->tasks) {
      CHECK_NOTNULL(task);
      tasks.push_back(task);
    }
    foreach (const memory::shared_ptr<Task>& task, framework->completedTasks) {
      tasks.push_back(task.get());
    }
  }

  // Sort tasks by task status timestamp. Default order is descending.
  // The earlist timestamp is chosen for comparison when multiple are present.
  Option<string> order = request.query.get("order");
  if (order.isSome() && (order.get() == "asc")) {
    sort(tasks.begin(), tasks.end(), TaskComparator::ascending);
  } else {
    sort(tasks.begin(), tasks.end(), TaskComparator::descending);
  }

  JSON::Array array;
  size_t end = std::min(offset + limit, tasks.size());
  for (size_t i = offset; i < end; i++) {
    const Task* task = tasks[i];
    array.values.push_back(model(*task));
  }

  JSON::Object object;
  object.values["tasks"] = array;

  return OK(object, request.query.get("jsonp"));
}


} // namespace master {
} // namespace internal {
} // namespace mesos {
