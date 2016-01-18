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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/resources.hpp>

#include <stout/foreach.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include "common/http.hpp"

#include "messages/messages.hpp"

using std::map;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

string serialize(
    ContentType contentType,
    const google::protobuf::Message& message)
{
  switch (contentType) {
    case ContentType::PROTOBUF: {
      return message.SerializeAsString();
    }
    case ContentType::JSON: {
      JSON::Object object = JSON::Protobuf(message);
      return stringify(object);
    }
  }

  UNREACHABLE();
}


// TODO(bmahler): Kill these in favor of automatic Proto->JSON
// Conversion (when it becomes available).

JSON::Object model(const Resources& resources)
{
  JSON::Object object;
  object.values["cpus"] = 0;
  object.values["mem"] = 0;
  object.values["disk"] = 0;

  // To maintain backwards compatibility we exclude revocable
  // resources in the reporting.
  Resources nonRevocable = resources - resources.revocable();

  map<string, Value_Type> types = nonRevocable.types();

  foreachpair (const string& name, const Value_Type& type, types) {
    switch (type) {
      case Value::SCALAR:
        object.values[name] =
          nonRevocable.get<Value::Scalar>(name).get().value();
        break;
      case Value::RANGES:
        object.values[name] =
          stringify(nonRevocable.get<Value::Ranges>(name).get());
        break;
      case Value::SET:
        object.values[name] =
          stringify(nonRevocable.get<Value::Set>(name).get());
        break;
      default:
        LOG(FATAL) << "Unexpected Value type: " << type;
    }
  }

  return object;
}


JSON::Object model(const hashmap<string, Resources>& roleResources)
{
  JSON::Object object;

  foreachpair (const string& role, const Resources& resources, roleResources) {
    object.values[role] = model(resources);
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


JSON::Array model(const Labels& labels)
{
  JSON::Array array;
  array.values.reserve(labels.labels().size()); // MESOS-2353.
  foreach (const Label& label, labels.labels()) {
    array.values.push_back(JSON::Protobuf(label));
  }
  return array;
}


JSON::Object model(const NetworkInfo& info)
{
  JSON::Object object;

  if (info.has_ip_address()) {
    object.values["ip_address"] = info.ip_address();
  }

  if (info.groups().size() > 0) {
    JSON::Array array;
    array.values.reserve(info.groups().size()); // MESOS-2353.
    foreach (const string& group, info.groups()) {
      array.values.push_back(group);
    }
    object.values["groups"] = std::move(array);
  }

  if (info.has_labels()) {
    object.values["labels"] = std::move(model(info.labels()));
  }

  return object;
}


JSON::Object model(const ContainerStatus& status)
{
  JSON::Object object;

  if (status.network_infos().size() > 0) {
    JSON::Array array;
    array.values.reserve(status.network_infos().size()); // MESOS-2353.
    foreach (const NetworkInfo& info, status.network_infos()) {
      array.values.push_back(model(info));
    }
    object.values["network_infos"] = std::move(array);
  }

  return object;
}


// Returns a JSON object modeled on a TaskStatus.
JSON::Object model(const TaskStatus& status)
{
  JSON::Object object;
  object.values["state"] = TaskState_Name(status.state());
  object.values["timestamp"] = status.timestamp();

  if (status.has_labels()) {
    object.values["labels"] = std::move(model(status.labels()));
  }

  if (status.has_container_status()) {
    object.values["container_status"] = model(status.container_status());
  }

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

  {
    JSON::Array array;
    array.values.reserve(task.statuses().size()); // MESOS-2353.

    foreach (const TaskStatus& status, task.statuses()) {
      array.values.push_back(model(status));
    }
    object.values["statuses"] = std::move(array);
  }

  if (task.has_labels()) {
    object.values["labels"] = std::move(model(task.labels()));
  }

  if (task.has_discovery()) {
    object.values["discovery"] = JSON::Protobuf(task.discovery());
  }

  return object;
}


JSON::Object model(const CommandInfo& command)
{
  JSON::Object object;

  if (command.has_shell()) {
    object.values["shell"] = command.shell();
  }

  if (command.has_value()) {
    object.values["value"] = command.value();
  }

  JSON::Array argv;
  foreach (const string& arg, command.arguments()) {
    argv.values.push_back(arg);
  }
  object.values["argv"] = argv;

  if (command.has_environment()) {
    JSON::Object environment;
    JSON::Array variables;
    foreach (const Environment_Variable& variable,
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
  foreach (const CommandInfo_URI& uri, command.uris()) {
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
  object.values["name"] = executorInfo.name();
  object.values["data"] = executorInfo.data();
  object.values["framework_id"] = executorInfo.framework_id().value();
  object.values["command"] = model(executorInfo.command());
  object.values["resources"] = model(executorInfo.resources());
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

  {
    JSON::Array array;
    array.values.reserve(statuses.size()); // MESOS-2353.

    foreach (const TaskStatus& status, statuses) {
      array.values.push_back(model(status));
    }
    object.values["statuses"] = std::move(array);
  }

  if (task.has_labels()) {
    object.values["labels"] = std::move(model(task.labels()));
  }

  if (task.has_discovery()) {
    object.values["discovery"] = JSON::Protobuf(task.discovery());
  }

  return object;
}


void json(JSON::ObjectWriter* writer, const Attributes& attributes)
{
  foreach (const Attribute& attribute, attributes) {
    switch (attribute.type()) {
      case Value::SCALAR:
        writer->field(attribute.name(), attribute.scalar());
        break;
      case Value::RANGES:
        writer->field(attribute.name(), attribute.ranges());
        break;
      case Value::SET:
        writer->field(attribute.name(), attribute.set());
        break;
      case Value::TEXT:
        writer->field(attribute.name(), attribute.text());
        break;
      default:
        LOG(FATAL) << "Unexpected Value type: " << attribute.type();
    }
  }
}


void json(JSON::ObjectWriter* writer, const Task& task)
{
  writer->field("id", task.task_id().value());
  writer->field("name", task.name());
  writer->field("framework_id", task.framework_id().value());
  writer->field("executor_id", task.executor_id().value());
  writer->field("slave_id", task.slave_id().value());
  writer->field("state", TaskState_Name(task.state()));
  writer->field("resources", Resources(task.resources()));
  writer->field("statuses", task.statuses());

  if (task.has_labels()) {
    writer->field("labels", task.labels());
  }

  if (task.has_discovery()) {
    writer->field("discovery", task.discovery());
  }
}

}  // namespace internal {

void json(JSON::ObjectWriter* writer, const CommandInfo& command)
{
  if (command.has_shell()) {
    writer->field("shell", command.shell());
  }

  if (command.has_value()) {
    writer->field("value", command.value());
  }

  writer->field("argv", command.arguments());

  if (command.has_environment()) {
    writer->field("environment", command.environment());
  }

  writer->field("uris", [&command](JSON::ArrayWriter* writer) {
    foreach (const CommandInfo::URI& uri, command.uris()) {
      writer->element([&uri](JSON::ObjectWriter* writer) {
        writer->field("value", uri.value());
        writer->field("executable", uri.executable());
      });
    }
  });
}


void json(JSON::ObjectWriter* writer, const ContainerStatus& status)
{
  if (status.network_infos().size() > 0) {
    writer->field("network_infos", status.network_infos());
  }
}


void json(JSON::ObjectWriter* writer, const ExecutorInfo& executorInfo)
{
  writer->field("executor_id", executorInfo.executor_id().value());
  writer->field("name", executorInfo.name());
  writer->field("data", executorInfo.data());
  writer->field("framework_id", executorInfo.framework_id().value());
  writer->field("command", executorInfo.command());
  writer->field("resources", Resources(executorInfo.resources()));
}


void json(JSON::ArrayWriter* writer, const Labels& labels)
{
  json(writer, labels.labels());
}


void json(JSON::ObjectWriter* writer, const NetworkInfo& info)
{
  if (info.has_ip_address()) {
    writer->field("ip_address", info.ip_address());
  }

  if (info.groups().size() > 0) {
    writer->field("groups", info.groups());
  }

  if (info.has_labels()) {
    writer->field("labels", info.labels());
  }
}


void json(JSON::ObjectWriter* writer, const Resources& resources)
{
  hashmap<std::string, double> scalars = {{"cpus", 0}, {"mem", 0}, {"disk", 0}};
  hashmap<std::string, Value::Ranges> ranges;
  hashmap<std::string, Value::Set> sets;

  // To maintain backwards compatibility we exclude revocable
  // resources in the reporting.
  foreach (const Resource& resource, resources) {
    if (Resources::isRevocable(resource)) {
      continue;
    }
    switch (resource.type()) {
      case Value::SCALAR:
        scalars[resource.name()] += resource.scalar().value();
        break;
      case Value::RANGES:
        ranges[resource.name()] += resource.ranges();
        break;
      case Value::SET:
        sets[resource.name()] += resource.set();
        break;
      default:
        LOG(FATAL) << "Unexpected Value type: " << resource.type();
    }
  }

  json(writer, scalars);
  json(writer, ranges);
  json(writer, sets);
}


void json(JSON::ObjectWriter* writer, const TaskStatus& status)
{
  writer->field("state", TaskState_Name(status.state()));
  writer->field("timestamp", status.timestamp());

  if (status.has_labels()) {
    writer->field("labels", status.labels());
  }

  if (status.has_container_status()) {
    writer->field("container_status", status.container_status());
  }
}


void json(JSON::NumberWriter* writer, const Value::Scalar& scalar)
{
  writer->set(scalar.value());
}


void json(JSON::StringWriter* writer, const Value::Ranges& ranges)
{
  writer->append(stringify(ranges));
}


void json(JSON::StringWriter* writer, const Value::Set& set)
{
  writer->append(stringify(set));
}


void json(JSON::StringWriter* writer, const Value::Text& text)
{
  writer->append(text.value());
}

}  // namespace mesos {
