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

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "master/master.hpp"

namespace mesos {
namespace internal {
namespace master {

using process::Future;

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
  object.values["resources"] = model(slave.info.resources());
  object.values["attributes"] = model(slave.info.attributes());
  return object;
}


Future<Response> Master::Http::vars(const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  // TODO(benh): Consider separating collecting the actual vars we
  // want to display from rendering them. Trying to just create a
  // map<string, string> required a lot of calls to stringify (or
  // using an std::ostringstream) and didn't actually seem to be that
  // much more clear than just rendering directly.
  std::ostringstream out;

  out <<
    "version: " << MESOS_VERSION << "\n" <<
    "build_date: " << build::DATE << "\n" <<
    "build_user: " << build::USER << "\n" <<
    "build_flags: " << build::FLAGS << "\n";

  // TODO(benh): Output flags.
  return OK(out.str(), request.query.get("jsonp"));
}

Future<Response> Master::Http::redirect(const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  // If there's no leader, redirect to this master's base url.
  UPID pid = master.leader != UPID() ? master.leader : master.self();

  Try<string> hostname = net::getHostname(pid.ip);
  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  return TemporaryRedirect(
      "http://" + hostname.get() + ":" + stringify(pid.port));
}


Future<Response> Master::Http::stats(const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = (Clock::now() - master.startTime).secs();
  object.values["elected"] = master.elected; // Note: using int not bool.
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
    totalResources += slave->info.resources();
    usedResources += slave->resourcesInUse;
  }

  foreach (const Resource& resource, totalResources) {
    if (resource.type() == Value::SCALAR) {
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
  }

  return OK(object, request.query.get("jsonp"));
}


Future<Response> Master::Http::state(const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

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

  // TODO(benh): Use an Option for the leader PID.
  if (master.leader != UPID()) {
    object.values["leader"] = string(master.leader);
  }

  if (master.flags.log_dir.isSome()) {
    object.values["log_dir"] = master.flags.log_dir.get();
  }

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

} // namespace master {
} // namespace internal {
} // namespace mesos {
