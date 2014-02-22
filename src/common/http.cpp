#include "common/http.hpp"

#include <map>
#include <string>

#include <glog/logging.h>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "common/attributes.hpp"

#include "messages/messages.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

using std::map;
using std::string;

namespace mesos {
namespace internal {

// TODO(bmahler): Kill these in favor of automatic Proto->JSON Conversion (when
// it becomes available).

JSON::Object model(const Resources& resources)
{
  JSON::Object object;
  object.values["cpus"] = 0;
  object.values["mem"] = 0;
  object.values["disk"] = 0;

  const Option<double>& cpus = resources.cpus();
  if (cpus.isSome()) {
    object.values["cpus"] = cpus.get();
  }

  const Option<Bytes>& mem = resources.mem();
  if (mem.isSome()) {
    object.values["mem"] = mem.get().megabytes();
  }

  const Option<Bytes>& disk = resources.disk();
  if (disk.isSome()) {
    object.values["disk"] = disk.get().megabytes();
  }

  const Option<Value::Ranges>& ports = resources.ports();
  if (ports.isSome()) {
    object.values["ports"] = stringify(ports.get());
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

}  // namespace internal {
}  // namespace mesos {
