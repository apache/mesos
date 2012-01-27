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
#include <sstream>
#include <string>

#include "common/build.hpp"
#include "common/foreach.hpp"
#include "common/json.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"

#include "slave/http.hpp"
#include "slave/slave.hpp"

using process::Future;
using process::HttpResponse;
using process::HttpRequest;

using std::string;


namespace mesos {
namespace internal {
namespace slave {

// TODO(benh): Consider moving the modeling code some place else so
// that it can be shared between slave/http.cpp and master/http.cpp.


// Returns a JSON object modeled on a Resources.
JSON::Object model(const Resources& resources)
{
  // TODO(benh): Add all of the resources.
  Value::Scalar none;
  Value::Scalar cpus = resources.get("cpus", none);
  Value::Scalar mem = resources.get("mem", none);

  JSON::Object object;
  object.values["cpus"] = cpus.value();
  object.values["mem"] = mem.value();

  return object;
}


JSON::Object model(const Executor& executor)
{
  JSON::Object object;
  object.values["id"] = executor.id.value();
  object.values["uri"] = executor.info.uri();
  object.values["directory"] = executor.directory;
  object.values["resources"] = model(executor.resources);

  JSON::Array array;

  // TODO(benh): Send queued tasks also.
  foreachvalue (Task* task, executor.launchedTasks) {
    JSON::Object object;
    object.values["id"] = task->task_id().value();
    object.values["name"] = task->name();
    object.values["framework_id"] = task->framework_id().value();
    object.values["slave_id"] = task->slave_id().value();
    object.values["state"] = TaskState_Name(task->state());
    object.values["resources"] = model(task->resources());

    array.values.push_back(object);
  }

  object.values["tasks"] = array;

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

Future<HttpResponse> vars(
    const Slave& slave,
    const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  // TODO(benh): Consider separating collecting the actual vars we
  // want to display from rendering them. Trying to just create a
  // map<string, string> required a lot of calls to utils::stringify
  // (or using an std::ostringstream) and didn't actually seem to be
  // that much more clear than just rendering directly.
  std::ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " << build::USER << "\n" <<
    "build_flags " << build::FLAGS << "\n";

  // Also add the configuration values.
  foreachpair (const string& key, const string& value, slave.conf.getMap()) {
    out << key << " " << value << "\n";
  }

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


namespace json {

Future<HttpResponse> stats(
    const Slave& slave,
    const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = Clock::now() - slave.startTime;
  object.values["total_frameworks"] = slave.frameworks.size();
  object.values["started_tasks"] = slave.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = slave.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = slave.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = slave.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = slave.stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = slave.stats.validStatusUpdates;
  object.values["invalid_status_updates"] = slave.stats.invalidStatusUpdates;

  std::ostringstream out;

  JSON::render(out, object);

  HttpOKResponse response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Future<HttpResponse> state(
    const Slave& slave,
    const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["build_date"] = build::DATE;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = slave.startTime;
  object.values["id"] = slave.id.value();
  object.values["pid"] = string(slave.self());
  object.values["resources"] = model(slave.resources);

  JSON::Array array;

  // Model all of the frameworks.
  foreachvalue (Framework* framework, slave.frameworks) {
    array.values.push_back(model(*framework));
  }

  object.values["frameworks"] = array;

  std::ostringstream out;

  JSON::render(out, object);

  HttpOKResponse response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}

} // namespace json {
} // namespace http {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
