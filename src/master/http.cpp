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

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/build.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "master/http.hpp"
#include "master/master.hpp"

namespace mesos {
namespace internal {
namespace master {

using process::Future;

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Request;

using std::map;
using std::string;
using std::vector;

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


// Returns a JSON object modeled on a Task.
JSON::Object model(const Task& task)
{
  JSON::Object object;
  object.values["id"] = task.task_id().value();
  object.values["name"] = task.name();
  object.values["framework_id"] = task.framework_id().value();
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
  object.values["registered_time"] = framework.registeredTime;
  object.values["unregistered_time"] = framework.unregisteredTime;
  object.values["active"] = framework.active;
  object.values["resources"] = model(framework.resources);

  // TODO(benh): Consider making reregisteredTime an Option.
  if (framework.registeredTime != framework.reregisteredTime) {
    object.values["reregistered_time"] = framework.reregisteredTime;
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
  object.values["hostname"] = slave.info.hostname();
  object.values["webui_hostname"] = slave.info.webui_hostname();
  object.values["webui_port"] = slave.info.webui_port();
  object.values["registered_time"] = slave.registeredTime;
  object.values["resources"] = model(slave.info.resources());
  return object;
}


namespace http {

Future<Response> vars(
    const Master& master,
    const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

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

  OK response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


namespace json {

Future<Response> stats(
    const Master& master,
    const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = Clock::now() - master.startTime;
  object.values["elected"] = master.elected; // Note: using int not bool.
  object.values["total_schedulers"] = master.frameworks.size();
  object.values["active_schedulers"] = master.getActiveFrameworks().size();
  object.values["activated_slaves"] = master.slaveHostnamePorts.size();
  object.values["connected_slaves"] = master.slaves.size();
  object.values["staged_tasks"] = master.stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = master.stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = master.stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = master.stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = master.stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = master.stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = master.stats.validStatusUpdates;
  object.values["invalid_status_updates"] = master.stats.invalidStatusUpdates;

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

  std::ostringstream out;

  JSON::render(out, object);

  OK response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Future<Response> state(
    const Master& master,
    const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["build_date"] = build::DATE;
  object.values["build_time"] = build::TIME;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = master.startTime;
  object.values["id"] = master.info.id();
  object.values["pid"] = string(master.self());

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

    foreach (const Framework& framework, master.completedFrameworks) {
      array.values.push_back(model(framework));
    }

    object.values["completed_frameworks"] = array;
  }

  std::ostringstream out;

  JSON::render(out, object);

  OK response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Future<Response> log(
    const Master& master,
    const Request& request)
{
  VLOG(1) << "HTTP request for '" << request.path << "'";

  map<string, vector<string> > pairs =
    strings::pairs(request.query, ";&", "=");

  off_t offset = -1;
  ssize_t length = -1;
  string level = pairs.count("level") > 0 ? pairs["level"][0] : "INFO";

  if (pairs.count("offset") > 0 && pairs["offset"].size() > 0) {
    Try<off_t> result = numify<off_t>(pairs["offset"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'offset' value (\""
                   << pairs["offset"].back() << "\"): "
                   << result.error();
      return BadRequest();
    }
    offset = result.get();
  }

  if (pairs.count("length") > 0 && pairs["length"].size() > 0) {
    Try<ssize_t> result = numify<ssize_t>(pairs["length"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'length' value (\""
                   << pairs["length"].back() << "\"): "
                   << result.error();
      return BadRequest();
    }
    length = result.get();
  }

  if (master.flags.log_dir.isNone()) {
    return NotFound();
  }

  string path = master.flags.log_dir.get() + "/mesos-master." + level;

  Try<int> fd = os::open(path, O_RDONLY);

  if (fd.isError()) {
    LOG(WARNING) << "Failed to open log file at "
                 << path << ": " << fd.error();
    return InternalServerError();
  }

  off_t size = lseek(fd.get(), 0, SEEK_END);

  if (size == -1) {
    PLOG(WARNING) << "Failed to seek in the log";
    close(fd.get());
    return InternalServerError();
  }

  if (offset == -1) {
    offset = size;
  }

  if (length == -1) {
    length = size - offset;
  }

  JSON::Object object;

  if (offset < size) {
    // Seek to the offset we want to read from.
    if (lseek(fd.get(), offset, SEEK_SET) == -1) {
      PLOG(WARNING) << "Failed to seek in the log";
      close(fd.get());
      return InternalServerError();
    }

    // Read length bytes (or to EOF).
    char* temp = new char[length];

    length = ::read(fd.get(), temp, length);

    if (length == 0) {
      object.values["offset"] = offset;
      object.values["length"] = 0;
      delete[] temp;
    } else if (length == -1) {
      PLOG(WARNING) << "Failed to read from the log";
      delete[] temp;
      close(fd.get());
      return InternalServerError();
    } else {
      object.values["offset"] = offset;
      object.values["length"] = length;
      object.values["data"] = string(temp, length);
      delete[] temp;
    }
  } else {
    object.values["offset"] = size;
    object.values["length"] = 0;
  }

  close(fd.get());

  std::ostringstream out;

  JSON::render(out, object);

  OK response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = stringify(out.str().size());
  response.body = out.str().data();
  return response;
}

} // namespace json {
} // namespace http {
} // namespace master {
} // namespace internal {
} // namespace mesos {
