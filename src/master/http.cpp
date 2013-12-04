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

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/help.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/type_utils.hpp"
#include "common/protobuf_utils.hpp"

#include "logging/logging.hpp"

#include "master/master.hpp"

namespace mesos {
namespace internal {
namespace master {

using process::Future;
using process::HELP;
using process::TLDR;
using process::USAGE;

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::TemporaryRedirect;
using process::http::Response;
using process::http::Request;

using std::map;
using std::string;
using std::vector;

// TODO(bmahler): Kill these in favor of automatic Proto->JSON Conversion (when
// it becomes available).

// Returns a JSON object modeled on a Resources.
JSON::Object model(const Resources& resources)
{
  JSON::Object object;

  foreach (const Resource& resource, resources) {
    switch (resource.type()) {
      case Value::SCALAR:
        object.values[resource.name()] = resource.scalar().value();
        break;
      case Value::RANGES:
        object.values[resource.name()] = stringify(resource.ranges());
        break;
      case Value::SET:
        object.values[resource.name()] = stringify(resource.set());
        break;
      default:
        LOG(FATAL) << "Unexpected Value type: " << resource.type();
        break;
    }
  }

  return object;
}


JSON::Object model(const Attributes& attributes)
{
  JSON::Object object;

  foreach (const Attribute& attribute, attributes) {
    switch (attribute.type()) {
      case Value::SCALAR:
        object.values[attribute.name()] = attribute.scalar().value();
        break;
      case Value::RANGES:
        object.values[attribute.name()] = stringify(attribute.ranges());
        break;
      case Value::SET:
        object.values[attribute.name()] = stringify(attribute.set());
        break;
      case Value::TEXT:
        object.values[attribute.name()] = attribute.text().value();
        break;
      default:
        LOG(FATAL) << "Unexpected Value type: " << attribute.type();
        break;
    }
  }

  return object;
}


// Returns a JSON object modeled on a TaskStatus.
JSON::Object model(const TaskStatus& status)
{
  JSON::Object object;
  object.values["state"] = TaskState_Name(status.state());
  object.values["timestamp"] = status.timestamp();

  return object;
}


// Returns a JSON object modeled on a Task.
// TODO(bmahler): Expose the executor name / source.
JSON::Object model(const Task& task)
{
  JSON::Object object;
  object.values["id"] = task.task_id().value();
  object.values["name"] = task.name();
  object.values["framework_id"] = task.framework_id().value();
  object.values["executor_id"] = task.executor_id().value();
  object.values["slave_id"] = task.slave_id().value();
  object.values["state"] = TaskState_Name(task.state());
  object.values["resources"] = model(task.resources());

  JSON::Array array;
  foreach (const TaskStatus& status, task.statuses()) {
    array.values.push_back(model(status));
  }
  object.values["statuses"] = array;

  return object;
}


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
  object.values["registered_time"] = framework.registeredTime.secs();
  object.values["unregistered_time"] = framework.unregisteredTime.secs();
  object.values["active"] = framework.active;
  object.values["resources"] = model(framework.resources);

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
    foreach (const Task& task, framework.completedTasks) {
      array.values.push_back(model(task));
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
  UPID pid = master.leader.isSome() ? master.leader.get() : master.self();

  Try<string> hostname = net::getHostname(pid.ip);
  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  return TemporaryRedirect(
      "http://" + hostname.get() + ":" + stringify(pid.port));
}


Future<Response> Master::Http::stats(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = (Clock::now() - master.startTime).secs();
  object.values["elected"] = master.elected(); // Note: using int not bool.
  object.values["total_schedulers"] = master.frameworks.size();
  object.values["active_schedulers"] = master.getActiveFrameworks().size();
  object.values["activated_slaves"] = master.slaves.size();
  object.values["deactivated_slaves"] = master.deactivatedSlaves.size();
  object.values["staged_tasks"] = master.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master.stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = master.stats.validStatusUpdates;
  object.values["invalid_status_updates"] = master.stats.invalidStatusUpdates;
  object.values["outstanding_offers"] = master.offers.size();

  // Get total and used (note, not offered) resources in order to
  // compute capacity of scalar resources.
  Resources totalResources;
  Resources usedResources;
  foreachvalue (Slave* slave, master.slaves) {
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

  return OK(object, request.query.get("jsonp"));
}


Future<Response> Master::Http::state(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["version"] = MESOS_VERSION;
  object.values["build_date"] = build::DATE;
  object.values["build_time"] = build::TIME;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = master.startTime.secs();
  object.values["id"] = master.info.id();
  object.values["pid"] = string(master.self());
  object.values["activated_slaves"] = master.slaves.size();
  object.values["deactivated_slaves"] = master.deactivatedSlaves.size();
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
    object.values["leader"] = string(master.leader.get());
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
    foreachvalue (Slave* slave, master.slaves) {
      array.values.push_back(model(*slave));
    }

    object.values["slaves"] = array;
  }

  // Model all of the frameworks.
  {
    JSON::Array array;
    foreachvalue (Framework* framework, master.frameworks) {
      array.values.push_back(model(*framework));
    }

    object.values["frameworks"] = array;
  }

  // Model all of the completed frameworks.
  {
    JSON::Array array;

    foreach (const std::tr1::shared_ptr<Framework>& framework,
             master.completedFrameworks) {
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

} // namespace master {
} // namespace internal {
} // namespace mesos {
