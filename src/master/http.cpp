#include <iomanip>
#include <sstream>
#include <string>

#include "common/build.hpp"
#include "common/foreach.hpp"
#include "common/json.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"

#include "master/http.hpp"
#include "master/master.hpp"

using process::HttpResponse;
using process::HttpRequest;
using process::Promise;

using std::string;


namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Consider moving the modeling code some place else so
// that it can be shared between slave/http.cpp and master/http.cpp.


// Returns a JSON object modeled on a Resources.
JSON::Object model(const Resources& resources)
{
  // TODO(benh): Add all of the resources.
  Resource::Scalar none;
  Resource::Scalar cpus = resources.get("cpus", none);
  Resource::Scalar mem = resources.get("mem", none);

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
  object.values["executor_uri"] = framework.info.executor().uri();
  object.values["connect_time"] = framework.registeredTime;
  object.values["resources"] = model(framework.resources);

  // Model all of the tasks associated with a framework.
  {
    JSON::Array array;
    foreachvalue (Task* task, framework.tasks) {
      array.values.push_back(model(*task));
    }

    object.values["tasks"] = array;
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
  object.values["web_ui_url"] = slave.info.public_hostname();
  object.values["connect_time"] = slave.registeredTime;
  object.values["resources"] = model(slave.info.resources());
  return object;
}


namespace http {

Promise<HttpResponse> vars(
    const Master& master,
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
  foreachpair (const string& key, const string& value, master.conf.getMap()) {
    out << key << " " << value << "\n";
  }

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


namespace json {

Promise<HttpResponse> stats(
    const Master& master,
    const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = Clock::now() - master.startTime;
  object.values["elected"] = master.elected; // Note: using int not bool.
  object.values["total_schedulers"] = master.frameworks.size();
  object.values["active_schedulers"] = master.getActiveFrameworks().size();
  object.values["activated_slaves"] = master.slaveHostnamePorts.size();
  object.values["connected_slaves"] = master.slaves.size();
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
  foreach (Slave* slave, master.getActiveSlaves()) {
    totalResources += slave->info.resources();
    usedResources += slave->resourcesInUse;
  }

  foreach (const Resource& resource, totalResources) {
    if (resource.type() == Resource::SCALAR) {
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

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> state(
    const Master& master,
    const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["build_date"] = build::DATE;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = master.startTime;
  object.values["id"] = master.id;
  object.values["pid"] = string(master.self());

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

  std::ostringstream out;

  JSON::render(out, object);

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}

} // namespace json {
} // namespace http {
} // namespace master {
} // namespace internal {
} // namespace mesos {
