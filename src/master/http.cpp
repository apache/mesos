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

#include <stout/base64.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "authorizer/authorizer.hpp"

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
using process::http::Unauthorized;

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

  // TODO(bmahler): Consider deprecating this in favor of the split
  // used and offered resources below.
  object.values["resources"] =
    model(framework.usedResources + framework.offeredResources);

  // TODO(bmahler): Use these in the webui.
  object.values["used_resources"] = model(framework.usedResources);
  object.values["offered_resources"] = model(framework.offeredResources);

  object.values["hostname"] = framework.info.hostname();
  object.values["webui_url"] = framework.info.webui_url();

  // TODO(benh): Consider making reregisteredTime an Option.
  if (framework.registeredTime != framework.reregisteredTime) {
    object.values["reregistered_time"] = framework.reregisteredTime.secs();
  }

  // Model all of the tasks associated with a framework.
  {
    JSON::Array array;

    foreachvalue (const TaskInfo& task, framework.pendingTasks) {
      vector<TaskStatus> statuses;
      array.values.push_back(model(task, framework.id, TASK_STAGING, statuses));
    }

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
  object.values["active"] = slave.active;
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
  MasterInfo info = master->leader.isSome()
    ? master->leader.get()
    : master->info_;

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


// TODO(alexandra.sava): Add stats for registered and removed slaves.
Future<Response> Master::Http::stats(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = (Clock::now() - master->startTime).secs();
  object.values["elected"] = master->elected() ? 1 : 0;
  object.values["total_schedulers"] = master->frameworks.registered.size();
  object.values["active_schedulers"] = master->_frameworks_active();
  object.values["activated_slaves"] = master->_slaves_active();
  object.values["deactivated_slaves"] = master->_slaves_inactive();
  object.values["outstanding_offers"] = master->offers.size();

  // NOTE: These are monotonically increasing counters.
  object.values["staged_tasks"] = master->stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master->stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master->stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master->stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master->stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master->stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = master->stats.validStatusUpdates;
  object.values["invalid_status_updates"] = master->stats.invalidStatusUpdates;

  // Get a count of all active tasks in the cluster i.e., the tasks
  // that are launched (TASK_STAGING, TASK_STARTING, TASK_RUNNING) but
  // haven't reached terminal state yet.
  // NOTE: This is a gauge representing an instantaneous value.
  int active_tasks = 0;
  foreachvalue (Framework* framework, master->frameworks.registered) {
    active_tasks += framework->tasks.size();
  }
  object.values["active_tasks_gauge"] = active_tasks;

  // Get total and used (note, not offered) resources in order to
  // compute capacity of scalar resources.
  Resources totalResources;
  Resources usedResources;
  foreachvalue (Slave* slave, master->slaves.registered) {
    // Instead of accumulating all types of resources (which is
    // not necessary), we only accumulate scalar resources. This
    // helps us bypass a performance problem caused by range
    // additions (e.g. ports).
    foreach (const Resource& resource, slave->info.resources()) {
      if (resource.type() == Value::SCALAR) {
        totalResources += resource;
      }
    }
    foreachvalue (const Resources& resources, slave->usedResources) {
      foreach (const Resource& resource, resources) {
        if (resource.type() == Value::SCALAR) {
          usedResources += resource;
        }
      }
    }
  }

  foreach (const Resource& resource, totalResources) {
    CHECK(resource.has_scalar());

    double total = resource.scalar().value();
    object.values[resource.name() + "_total"] = total;

    Option<Value::Scalar> _used =
      usedResources.get<Value::Scalar>(resource.name());

    double used = _used.isSome() ? _used.get().value() : 0.0;
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
  object.values["start_time"] = master->startTime.secs();

  if (master->electedTime.isSome()) {
    object.values["elected_time"] = master->electedTime.get().secs();
  }

  object.values["id"] = master->info().id();
  object.values["pid"] = string(master->self());
  object.values["hostname"] = master->info().hostname();
  object.values["activated_slaves"] = master->_slaves_active();
  object.values["deactivated_slaves"] = master->_slaves_inactive();
  object.values["staged_tasks"] = master->stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master->stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master->stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master->stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master->stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master->stats.tasks[TASK_LOST];

  if (master->flags.cluster.isSome()) {
    object.values["cluster"] = master->flags.cluster.get();
  }

  if (master->leader.isSome()) {
    object.values["leader"] = master->leader.get().pid();
  }

  if (master->flags.log_dir.isSome()) {
    object.values["log_dir"] = master->flags.log_dir.get();
  }

  if (master->flags.external_log_file.isSome()) {
    object.values["external_log_file"] = master->flags.external_log_file.get();
  }

  JSON::Object flags;
  foreachpair (const string& name, const flags::Flag& flag, master->flags) {
    Option<string> value = flag.stringify(master->flags);
    if (value.isSome()) {
      flags.values[name] = value.get();
    }
  }
  object.values["flags"] = flags;

  // Model all of the slaves.
  {
    JSON::Array array;
    foreachvalue (Slave* slave, master->slaves.registered) {
      array.values.push_back(model(*slave));
    }

    object.values["slaves"] = array;
  }

  // Model all of the frameworks.
  {
    JSON::Array array;
    foreachvalue (Framework* framework, master->frameworks.registered) {
      array.values.push_back(model(*framework));
    }

    object.values["frameworks"] = array;
  }

  // Model all of the completed frameworks.
  {
    JSON::Array array;

    foreach (const memory::shared_ptr<Framework>& framework,
             master->frameworks.completed) {
      array.values.push_back(model(*framework));
    }

    object.values["completed_frameworks"] = array;
  }

  // Model all of the orphan tasks.
  {
    JSON::Array array;

    // Find those orphan tasks.
    foreachvalue (const Slave* slave, master->slaves.registered) {
      typedef hashmap<TaskID, Task*> TaskMap;
      foreachvalue (const TaskMap& tasks, slave->tasks) {
        foreachvalue (const Task* task, tasks) {
          CHECK_NOTNULL(task);
          if (!master->frameworks.registered.contains(task->framework_id())) {
            array.values.push_back(model(*task));
          }
        }
      }
    }

    object.values["orphan_tasks"] = array;
  }

  // Model all currently unregistered frameworks.
  // This could happen when the framework has yet to re-register
  // after master failover.
  {
    JSON::Array array;

    // Find unregistered frameworks.
    foreachvalue (const Slave* slave, master->slaves.registered) {
      foreachkey (const FrameworkID& frameworkId, slave->tasks) {
        if (!master->frameworks.registered.contains(frameworkId)) {
          array.values.push_back(frameworkId.value());
        }
      }
    }

    object.values["unregistered_frameworks"] = array;
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
    foreachvalue (Role* role, master->roles) {
      array.values.push_back(model(*role));
    }

    object.values["roles"] = array;
  }

  return OK(object, request.query.get("jsonp"));
}


const string Master::Http::SHUTDOWN_HELP = HELP(
    TLDR(
        "Shuts down a running framework."),
    USAGE(
        "/master/shutdown"),
    DESCRIPTION(
        "Please provide a \"frameworkId\" value designating the ",
        "running framework to shut down.",
        "Returns 200 OK if the framework was correctly shutdown."));


Future<Response> Master::Http::shutdown(const Request& request)
{
  if (request.method != "POST") {
    return BadRequest("Expecting POST");
  }

  // Parse the query string in the request body (since this is a POST)
  // in order to determine the framework ID to shutdown.
  hashmap<string, string> values =
    process::http::query::parse(request.body);

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
    return _shutdown(id);
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

  lambda::function<Future<Response>(bool)> _shutdown =
    lambda::bind(&Master::Http::_shutdown, this, id, lambda::_1);

  return master->authorizer.get()->authorize(shutdown)
    .then(defer(master->self(), _shutdown));
}


Future<Response> Master::Http::_shutdown(
    const FrameworkID& id,
    bool authorized)
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
  foreachvalue (Framework* framework, master->frameworks.registered) {
    frameworks.push_back(framework);
  }
  foreach (const memory::shared_ptr<Framework>& framework,
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


Result<Credential> Master::Http::authenticate(const Request& request)
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

  const string& decoded =
    base64::decode(strings::split(authorization.get(), " ", 2)[1]);

  const vector<string>& pairs = strings::split(decoded, ":", 2);

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
