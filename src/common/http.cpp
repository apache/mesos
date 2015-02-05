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

#include <vector>

#include <mesos/resources.hpp>

#include <stout/foreach.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include "common/attributes.hpp"
#include "common/http.hpp"

#include "messages/messages.hpp"

using std::vector;

namespace mesos {
namespace internal {

// TODO(bmahler): Kill these in favor of automatic Proto->JSON
// Conversion (when it becomes available).

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

  if (task.has_executor_id()) {
    object.values["executor_id"] = task.executor_id().value();
  } else {
    object.values["executor_id"] = "";
  }

  object.values["slave_id"] = task.slave_id().value();
  object.values["state"] = TaskState_Name(task.state());
  object.values["resources"] = model(task.resources());

  JSON::Array array;
  foreach (const TaskStatus& status, task.statuses()) {
    array.values.push_back(model(status));
  }
  object.values["statuses"] = array;

  JSON::Array labels;
  if (task.has_labels()) {
    foreach (const Label& label, task.labels().labels()) {
      labels.values.push_back(JSON::Protobuf(label));
    }
  }
  object.values["labels"] = labels;

  if (task.has_discovery()) {
    object.values["discovery"] = JSON::Protobuf(task.discovery());
  }

  return object;
}


// TODO(bmahler): Expose the executor name / source.
JSON::Object model(
    const TaskInfo& task,
    const FrameworkID& frameworkId,
    const TaskState& state,
    const vector<TaskStatus>& statuses)
{
  JSON::Object object;
  object.values["id"] = task.task_id().value();
  object.values["name"] = task.name();
  object.values["framework_id"] = frameworkId.value();

  if (task.has_executor()) {
    object.values["executor_id"] = task.executor().executor_id().value();
  } else {
    object.values["executor_id"] = "";
  }

  object.values["slave_id"] = task.slave_id().value();
  object.values["state"] = TaskState_Name(state);
  object.values["resources"] = model(task.resources());

  JSON::Array array;
  foreach (const TaskStatus& status, statuses) {
    array.values.push_back(model(status));
  }
  object.values["statuses"] = array;

  JSON::Array labels;
  if (task.has_labels()) {
    foreach (const Label& label, task.labels().labels()) {
      labels.values.push_back(JSON::Protobuf(label));
    }
  }
  object.values["labels"] = labels;

  if (task.has_discovery()) {
    object.values["discovery"] = JSON::Protobuf(task.discovery());
  }

  return object;
}


}  // namespace internal {
}  // namespace mesos {
