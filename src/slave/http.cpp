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

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "slave/http.hpp"
#include "slave/slave.hpp"

namespace mesos {
namespace internal {
namespace slave {

using process::Future;

using process::http::OK;
using process::http::Response;
using process::http::Request;

using std::map;
using std::string;
using std::vector;


// TODO(bmahler): Kill these in favor of automatic Proto->JSON Conversion (when
// in becomes available).


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


JSON::Object model(const CommandInfo& command)
{
  JSON::Object object;
  object.values["value"] = command.value();

  if (command.has_environment()) {
    JSON::Object environment;
    JSON::Array variables;
    foreach(const Environment_Variable& variable,
        command.environment().variables()) {
      JSON::Object variableObject;
      variableObject.values["name"] = variable.name();
      variableObject.values["value"] = variable.value();
      variables.values.push_back(variableObject);
    }
    environment.values["variables"] = variables;
    object.values["environment"] = environment;
  }

  JSON::Array uris;
  foreach(const CommandInfo_URI& uri, command.uris()) {
    JSON::Object uriObject;
    uriObject.values["value"] = uri.value();
    uriObject.values["executable"] = uri.executable();

    uris.values.push_back(uriObject);
  }
  object.values["uris"] = uris;

  return object;
}


JSON::Object model(const ExecutorInfo& executorInfo)
{
  JSON::Object object;
  object.values["executor_id"] = executorInfo.executor_id().value();
  object.values["data"] = executorInfo.data();
  object.values["framework_id"] = executorInfo.framework_id().value();
  object.values["command"] = model(executorInfo.command());
  object.values["resources"] = model(executorInfo.resources());
  return object;
}


JSON::Object model(const Executor& executor)
{
  JSON::Object object;
  object.values["id"] = executor.id.value();
  object.values["directory"] = executor.directory;
  object.values["resources"] = model(executor.resources);

  JSON::Array tasks;
  foreachvalue (Task* task, executor.launchedTasks) {
    JSON::Object object;
    object.values["id"] = task->task_id().value();
    object.values["name"] = task->name();
    object.values["executor_id"] = task->executor_id().value();
    object.values["framework_id"] = task->framework_id().value();
    object.values["slave_id"] = task->slave_id().value();
    object.values["state"] = TaskState_Name(task->state());
    object.values["resources"] = model(task->resources());

    tasks.values.push_back(object);
  }
  object.values["tasks"] = tasks;

  JSON::Array queued;
  foreachvalue (const TaskInfo& task, executor.queuedTasks) {
    JSON::Object object;
    object.values["id"] = task.task_id().value();
    object.values["name"] = task.name();
    object.values["slave_id"] = task.slave_id().value();
    object.values["resources"] = model(task.resources());
    object.values["data"] = task.data();

    if (task.has_command()) {
      object.values["command"] = model(task.command());
    }
    if (task.has_executor()) {
      object.values["executor_id"] = model(task.executor());
    }

    queued.values.push_back(object);
  }
  object.values["queued_tasks"] = queued;

  return object;
}


// Returns a JSON object modeled after a Framework.
JSON::Object model(const Framework& framework)
{
  JSON::Object object;
  object.values["id"] = framework.id.value();
  object.values["name"] = framework.info.name();
  object.values["user"] = framework.info.user();

  JSON::Array array;

  // Model all of the executors.
  foreachvalue (Executor* executor, framework.executors) {
    array.values.push_back(model(*executor));
  }

  object.values["executors"] = array;

  return object;
}


namespace http {

Future<Response> vars(
    const Slave& slave,
    const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  // TODO(benh): Consider separating collecting the actual vars we
  // want to display from rendering them. Trying to just create a
  // map<string, string> required a lot of calls to stringify (or
  // using an std::ostringstream) and didn't actually seem to be that
  // much more clear than just rendering directly.
  std::ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " << build::USER << "\n" <<
    "build_flags " << build::FLAGS << "\n";

  // TODO(benh): Output flags.
  return OK(out.str(), request.query.get("jsonp"));
}


namespace json {

Future<Response> stats(
    const Slave& slave,
    const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = Clock::now() - slave.startTime;
  object.values["total_frameworks"] = slave.frameworks.size();
  object.values["staged_tasks"] = slave.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = slave.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = slave.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = slave.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = slave.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = slave.stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = slave.stats.validStatusUpdates;
  object.values["invalid_status_updates"] = slave.stats.invalidStatusUpdates;

  return OK(object, request.query.get("jsonp"));
}


Future<Response> state(
    const Slave& slave,
    const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["build_date"] = build::DATE;
  object.values["build_time"] = build::TIME;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = slave.startTime;
  object.values["id"] = slave.id.value();
  object.values["pid"] = string(slave.self());
  object.values["hostname"] = slave.info.hostname();
  object.values["resources"] = model(slave.resources);
  object.values["attributes"] = model(slave.attributes);
  object.values["staged_tasks"] = slave.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = slave.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = slave.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = slave.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = slave.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = slave.stats.tasks[TASK_LOST];

  Try<string> masterHostname = net::getHostname(slave.master.ip);
  if (masterHostname.isSome()) {
    object.values["master_hostname"] = masterHostname.get();
  }

  if (slave.flags.log_dir.isSome()) {
    object.values["log_dir"] = slave.flags.log_dir.get();
  }

  JSON::Array array;

  // Model all of the frameworks.
  foreachvalue (Framework* framework, slave.frameworks) {
    array.values.push_back(model(*framework));
  }

  object.values["frameworks"] = array;
  return OK(object, request.query.get("jsonp"));
}

} // namespace json {
} // namespace http {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
