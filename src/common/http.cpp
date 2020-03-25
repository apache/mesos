// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/repeated_field.h>

#include <mesos/attributes.hpp>
#include <mesos/http.hpp>
#include <mesos/resources.hpp>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>
#include <mesos/authentication/http/combined_authenticator.hpp>
#include <mesos/authorizer/authorizer.hpp>
#include <mesos/module/http_authenticator.hpp>
#include <mesos/quota/quota.hpp>

#include <process/authenticator.hpp>
#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/permissions.hpp>

#include "common/authorization.hpp"
#include "common/http.hpp"

#include "messages/messages.hpp"
#include "module/manager.hpp"

using std::map;
using std::ostream;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::Failure;
using process::Owned;

#ifdef USE_SSL_SOCKET
using process::http::authentication::JWTAuthenticator;
#endif // USE_SSL_SOCKET
using process::http::authentication::Principal;

using process::http::authorization::AuthorizationCallbacks;

using mesos::http::authentication::BasicAuthenticatorFactory;
using mesos::http::authentication::CombinedAuthenticator;

namespace mesos {

ostream& operator<<(ostream& stream, ContentType contentType)
{
  switch (contentType) {
    case ContentType::PROTOBUF: {
      return stream << APPLICATION_PROTOBUF;
    }
    case ContentType::JSON: {
      return stream << APPLICATION_JSON;
    }
    case ContentType::RECORDIO: {
      return stream << APPLICATION_RECORDIO;
    }
  }

  UNREACHABLE();
}

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
      return jsonify(JSON::Protobuf(message));
    }
    case ContentType::RECORDIO: {
      LOG(FATAL) << "Serializing a RecordIO stream is not supported";
    }
  }

  UNREACHABLE();
}


bool streamingMediaType(ContentType contentType)
{
  switch(contentType) {
    case ContentType::PROTOBUF:
    case ContentType::JSON: {
      return false;
    }

    case ContentType::RECORDIO: {
      return true;
    }
  }

  UNREACHABLE();
}


string lowerSlaveToAgent(string s)
{
  size_t index = 0;

  while (true) {
    index = s.find("slave", index);
    if (index == std::string::npos) break;
    s.replace(index, 5, "agent");
    index += 5;
  }

  return s;
}


string upperSlaveToAgent(string s)
{
  size_t index = 0;

  while (true) {
    index = s.find("SLAVE", index);
    if (index == std::string::npos) break;
    s.replace(index, 5, "AGENT");
    index += 5;
  }

  return s;
}


// This is a copy of the version in stout:
//
//   https://github.com/apache/mesos/blob/1.9.0/3rdparty/
//     stout/include/stout/protobuf.hpp#L816
//
// Except that we remap "slave" to "agent" in field names and
// enum values.
//
// Therefore, this is pretty brittle! If we change the one in
// stout we'll likely not realize this one should get a similar
// change. Note however that this can be removed if we were to
// keep up-to-date v1 state related objects in the master per
// MESOS-10040.
//
// Hopefully keeping a copy of the stout code doesn't prove
// too big of an issue since the stout version is pretty
// stable. Tickets to change the stout code (MESOS-3449,
// MESOS-6568, and MESOS-8727 have been updated with a note
// to change this copy as well).
void json(JSON::ObjectWriter* writer, const asV1Protobuf& protobuf)
{
  using google::protobuf::FieldDescriptor;

  const google::protobuf::Message& message = protobuf;

  const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
  const google::protobuf::Reflection* reflection = message.GetReflection();

  // We first look through all the possible fields to determine both the set
  // fields __and__ the optional fields with a default that are not set.
  // `Reflection::ListFields()` alone will only include set fields and
  // is therefore insufficient.
  int fieldCount = descriptor->field_count();
  std::vector<const FieldDescriptor*> fields;
  fields.reserve(fieldCount);
  for (int i = 0; i < fieldCount; ++i) {
    const FieldDescriptor* field = descriptor->field(i);
    if (field->is_repeated()) { // Repeated or Map.
      if (reflection->FieldSize(message, field) > 0) {
        // Has repeated field with members, output as JSON.
        fields.push_back(field);
      }
    } else if (
        reflection->HasField(message, field) ||
        (field->has_default_value() && !field->options().deprecated())) {
      // Field is set or has default, output as JSON.
      fields.push_back(field);
    }
  }

  foreach (const FieldDescriptor* field, fields) {
    if (field->is_repeated() && !field->is_map()) {
      writer->field(
          lowerSlaveToAgent(field->name()),
          [&field, &reflection, &message](JSON::ArrayWriter* writer) {
            int fieldSize = reflection->FieldSize(message, field);
            for (int i = 0; i < fieldSize; ++i) {
              switch (field->cpp_type()) {
                case FieldDescriptor::CPPTYPE_BOOL:
                  writer->element(
                      reflection->GetRepeatedBool(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_INT32:
                  writer->element(
                      reflection->GetRepeatedInt32(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_INT64:
                  writer->element(
                      reflection->GetRepeatedInt64(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_UINT32:
                  writer->element(
                      reflection->GetRepeatedUInt32(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_UINT64:
                  writer->element(
                      reflection->GetRepeatedUInt64(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_FLOAT:
                  writer->element(
                      reflection->GetRepeatedFloat(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_DOUBLE:
                  writer->element(
                      reflection->GetRepeatedDouble(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_MESSAGE:
                  writer->element(
                      asV1Protobuf(
                          reflection->GetRepeatedMessage(message, field, i)));
                  break;
                case FieldDescriptor::CPPTYPE_ENUM:
                  writer->element(
                      upperSlaveToAgent(
                          reflection->GetRepeatedEnum(message, field, i)
                            ->name()));
                  break;
                case FieldDescriptor::CPPTYPE_STRING:
                  const std::string& s = reflection->GetRepeatedStringReference(
                      message, field, i, nullptr);
                  if (field->type() == FieldDescriptor::TYPE_BYTES) {
                    writer->element(base64::encode(s));
                  } else {
                    writer->element(s);
                  }
                  break;
              }
            }
          });
    } else { // field->is_map() || !field->is_repeated()
      auto writeField = [&writer](
                            const std::string& fieldName,
                            const google::protobuf::Reflection* reflection,
                            const google::protobuf::Message& message,
                            const FieldDescriptor* field) {
        switch (field->cpp_type()) {
          case FieldDescriptor::CPPTYPE_BOOL:
            writer->field(fieldName, reflection->GetBool(message, field));
            break;
          case FieldDescriptor::CPPTYPE_INT32:
            writer->field(fieldName, reflection->GetInt32(message, field));
            break;
          case FieldDescriptor::CPPTYPE_INT64:
            writer->field(fieldName, reflection->GetInt64(message, field));
            break;
          case FieldDescriptor::CPPTYPE_UINT32:
            writer->field(fieldName, reflection->GetUInt32(message, field));
            break;
          case FieldDescriptor::CPPTYPE_UINT64:
            writer->field(fieldName, reflection->GetUInt64(message, field));
            break;
          case FieldDescriptor::CPPTYPE_FLOAT:
            writer->field(fieldName, reflection->GetFloat(message, field));
            break;
          case FieldDescriptor::CPPTYPE_DOUBLE:
            writer->field(fieldName, reflection->GetDouble(message, field));
            break;
          case FieldDescriptor::CPPTYPE_MESSAGE:
            writer->field(
                fieldName,
                asV1Protobuf(reflection->GetMessage(message, field)));
            break;
          case FieldDescriptor::CPPTYPE_ENUM:
            writer->field(
                fieldName,
                upperSlaveToAgent(reflection->GetEnum(message, field)->name()));
            break;
          case FieldDescriptor::CPPTYPE_STRING:
            const std::string& s =
              reflection->GetStringReference(message, field, nullptr);
            if (field->type() == FieldDescriptor::TYPE_BYTES) {
              writer->field(fieldName, base64::encode(s));
            } else {
              writer->field(fieldName, s);
            }
            break;
        }
      };

      if (!field->is_repeated()) { // Singular field.
        writeField(
            lowerSlaveToAgent(field->name()), reflection, message, field);
      } else { // Map field.
        CHECK(field->is_map());
        writer->field(
            lowerSlaveToAgent(field->name()),
            [&field, &reflection, &message, &writeField](
                JSON::ObjectWriter* writer) {
              foreach (
                  const auto& mapEntry,
                  reflection->GetRepeatedFieldRef<google::protobuf::Message>(
                      message, field)) {
                const google::protobuf::Descriptor* mapEntryDescriptor =
                  mapEntry.GetDescriptor();
                const google::protobuf::Reflection* mapEntryReflection =
                  mapEntry.GetReflection();

                // Map entry must contain exactly two fields: `key` and `value`.
                CHECK_EQ(mapEntryDescriptor->field_count(), 2);

                const FieldDescriptor* keyField = mapEntryDescriptor->field(0);
                const FieldDescriptor* valueField =
                  mapEntryDescriptor->field(1);

                switch (keyField->cpp_type()) {
                  case FieldDescriptor::CPPTYPE_BOOL:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetBool(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_INT32:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetInt32(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_INT64:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetInt64(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_UINT32:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetUInt32(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_UINT64:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetUInt64(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_STRING:
                    if (keyField->type() == FieldDescriptor::TYPE_BYTES) {
                      LOG(FATAL)
                        << "Unexpected key field type in protobuf Map: "
                        << keyField->type_name();
                    }

                    writeField(
                        mapEntryReflection->GetStringReference(
                            mapEntry, keyField, nullptr),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_FLOAT:
                  case FieldDescriptor::CPPTYPE_DOUBLE:
                  case FieldDescriptor::CPPTYPE_MESSAGE:
                  case FieldDescriptor::CPPTYPE_ENUM:
                    LOG(FATAL) << "Unexpected key field type in protobuf Map: "
                               << keyField->cpp_type_name();
                }
              }
            });
      }
    }
  }
}


// TODO(bmahler): Kill these in favor of automatic Proto->JSON
// Conversion (when it becomes available).

// Helper function that returns the JSON::value of a given resource (identified
// by 'name' and 'type') inside the resources.
static JSON::Value value(
    const string& name,
    const Value::Type& type,
    const Resources& resources)
{
  switch (type) {
    case Value::SCALAR:
      return resources.get<Value::Scalar>(name)->value();
    case Value::RANGES:
      return stringify(resources.get<Value::Ranges>(name).get());
    case Value::SET:
      return stringify(resources.get<Value::Set>(name).get());
    default:
      LOG(FATAL) << "Unexpected Value type: " << type;
  }

  UNREACHABLE();
}


JSON::Object model(const Resources& resources)
{
  JSON::Object object;
  object.values["cpus"] = 0;
  object.values["gpus"] = 0;
  object.values["mem"] = 0;
  object.values["disk"] = 0;

  // Model non-revocable resources.
  Resources nonRevocable = resources.nonRevocable();

  foreachpair (
      const string& name, const Value::Type& type, nonRevocable.types()) {
    object.values[name] = value(name, type, nonRevocable);
  }

  // Model revocable resources.
  Resources revocable = resources.revocable();

  foreachpair (const string& name, const Value::Type& type, revocable.types()) {
    object.values[name + "_revocable"] = value(name, type, revocable);
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
  return JSON::protobuf(labels.labels());
}


JSON::Object model(const NetworkInfo& info)
{
  JSON::Object object;

  if (info.groups().size() > 0) {
    JSON::Array array;
    array.values.reserve(info.groups().size()); // MESOS-2353.
    foreach (const string& group, info.groups()) {
      array.values.push_back(group);
    }
    object.values["groups"] = std::move(array);
  }

  if (info.has_labels()) {
    object.values["labels"] = model(info.labels());
  }

  if (info.ip_addresses().size() > 0) {
    JSON::Array array;
    array.values.reserve(info.ip_addresses().size()); // MESOS-2353.
    foreach (const NetworkInfo::IPAddress& ipAddress, info.ip_addresses()) {
      array.values.push_back(JSON::protobuf(ipAddress));
    }
    object.values["ip_addresses"] = std::move(array);
  }

  if (info.has_name()) {
    object.values["name"] = info.name();
  }

  if (info.port_mappings().size() > 0) {
    JSON::Array array;
    array.values.reserve(info.port_mappings().size()); // MESOS-2353
    foreach (const NetworkInfo::PortMapping& portMapping,
             info.port_mappings()) {
      array.values.push_back(JSON::protobuf(portMapping));
    }
    object.values["port_mappings"] = std::move(array);
  }

  return object;
}


JSON::Object model(const ContainerStatus& status)
{
  JSON::Object object;

  if (status.has_container_id()) {
    object.values["container_id"] = JSON::protobuf(status.container_id());
  }

  if (status.network_infos().size() > 0) {
    JSON::Array array;
    array.values.reserve(status.network_infos().size()); // MESOS-2353.
    foreach (const NetworkInfo& info, status.network_infos()) {
      array.values.push_back(model(info));
    }
    object.values["network_infos"] = std::move(array);
  }

  if (status.has_cgroup_info()) {
    object.values["cgroup_info"] = JSON::protobuf(status.cgroup_info());
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
    object.values["labels"] = model(status.labels());
  }

  if (status.has_container_status()) {
    object.values["container_status"] = model(status.container_status());
  }

  if (status.has_healthy()) {
    object.values["healthy"] = status.healthy();
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

  if (!task.limits().empty()) {
    object.values["limits"] = model(task.limits());
  }

  if (task.has_user()) {
    object.values["user"] = task.user();
  }

  {
    JSON::Array array;
    array.values.reserve(task.statuses().size()); // MESOS-2353.

    foreach (const TaskStatus& status, task.statuses()) {
      array.values.push_back(model(status));
    }
    object.values["statuses"] = std::move(array);
  }

  if (task.has_labels()) {
    object.values["labels"] = model(task.labels());
  }

  if (task.has_discovery()) {
    object.values["discovery"] = JSON::protobuf(task.discovery());
  }

  if (task.has_container()) {
    object.values["container"] = JSON::protobuf(task.container());
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
  object.values["framework_id"] = executorInfo.framework_id().value();
  object.values["command"] = model(executorInfo.command());
  object.values["resources"] = model(executorInfo.resources());

  if (executorInfo.has_labels()) {
    object.values["labels"] = model(executorInfo.labels());
  }

  return object;
}


// Returns JSON representation of a FileInfo protobuf message.
// Example JSON:
// {
//   'path': '\/some\/file',
//   'mode': '-rwxrwxrwx',
//   'nlink': 5,
//   'uid': 'bmahler',
//   'gid': 'employee',
//   'size': 4096,           // Bytes.
//   'mtime': 1348258116,    // Unix timestamp.
// }
JSON::Object model(const FileInfo& fileInfo)
{
  JSON::Object file;
  file.values["path"] = fileInfo.path();
  file.values["nlink"] = fileInfo.nlink();
  file.values["size"] = fileInfo.size();
  file.values["mtime"] = Nanoseconds(fileInfo.mtime().nanoseconds()).secs();

  char filetype;
  if (S_ISREG(fileInfo.mode())) {
    filetype = '-';
  } else if (S_ISDIR(fileInfo.mode())) {
    filetype = 'd';
  } else if (S_ISCHR(fileInfo.mode())) {
    filetype = 'c';
  } else if (S_ISBLK(fileInfo.mode())) {
    filetype = 'b';
  } else if (S_ISFIFO(fileInfo.mode())) {
    filetype = 'p';
  } else if (S_ISLNK(fileInfo.mode())) {
    filetype = 'l';
  } else if (S_ISSOCK(fileInfo.mode())) {
    filetype = 's';
  } else {
    filetype = '-';
  }

  struct os::Permissions permissions(fileInfo.mode());

  file.values["mode"] = strings::format(
      "%c%c%c%c%c%c%c%c%c%c",
      filetype,
      permissions.owner.r ? 'r' : '-',
      permissions.owner.w ? 'w' : '-',
      permissions.owner.x ? 'x' : '-',
      permissions.group.r ? 'r' : '-',
      permissions.group.w ? 'w' : '-',
      permissions.group.x ? 'x' : '-',
      permissions.others.r ? 'r' : '-',
      permissions.others.w ? 'w' : '-',
      permissions.others.x ? 'x' : '-').get();

  file.values["uid"] = fileInfo.uid();
  file.values["gid"] = fileInfo.gid();

  return file;
}


JSON::Object model(const google::protobuf::Map<string, Value_Scalar>& map)
{
  JSON::Object result, scalar;

  foreach (auto item, map) {
    result.values[item.first] = item.second.value();
  }

  return result;
}

}  // namespace internal {


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
    writer->field("environment", JSON::Protobuf(command.environment()));
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


static void json(JSON::ObjectWriter* writer, const ContainerStatus& status)
{
  if (status.has_container_id()) {
    writer->field("container_id", JSON::Protobuf(status.container_id()));
  }

  if (status.network_infos().size() > 0) {
    writer->field("network_infos", status.network_infos());
  }

  if (status.has_cgroup_info()) {
    writer->field("cgroup_info", JSON::Protobuf(status.cgroup_info()));
  }
}


static void json(
    JSON::ObjectWriter* writer,
    const DomainInfo::FaultDomain::RegionInfo& regionInfo)
{
  writer->field("name", regionInfo.name());
}


static void json(
    JSON::ObjectWriter* writer,
    const DomainInfo::FaultDomain::ZoneInfo& zoneInfo)
{
  writer->field("name", zoneInfo.name());
}


static void json(
    JSON::ObjectWriter* writer,
    const DomainInfo::FaultDomain& faultDomain)
{
    writer->field("region", faultDomain.region());
    writer->field("zone", faultDomain.zone());
}


void json(JSON::ObjectWriter* writer, const DomainInfo& domainInfo)
{
  if (domainInfo.has_fault_domain()) {
    writer->field("fault_domain", domainInfo.fault_domain());
  }
}


void json(JSON::ObjectWriter* writer, const ExecutorInfo& executorInfo)
{
  writer->field("executor_id", executorInfo.executor_id().value());
  writer->field("name", executorInfo.name());
  writer->field("framework_id", executorInfo.framework_id().value());
  writer->field("command", executorInfo.command());
  writer->field("resources", executorInfo.resources());

  // Resources may be empty for command executors.
  if (!executorInfo.resources().empty()) {
    // Executors are not allowed to mix resources allocated to
    // different roles, see MESOS-6636.
    writer->field(
        "role",
        executorInfo.resources().begin()->allocation_info().role());
  }

  if (executorInfo.has_labels()) {
    writer->field("labels", executorInfo.labels());
  }

  if (executorInfo.has_type()) {
    writer->field("type", ExecutorInfo::Type_Name(executorInfo.type()));
  }
}


void json(
    JSON::StringWriter* writer,
    const FrameworkInfo::Capability& capability)
{
  writer->set(FrameworkInfo::Capability::Type_Name(capability.type()));
}


void json(JSON::ArrayWriter* writer, const Labels& labels)
{
  foreach (const Label& label, labels.labels()) {
    writer->element(JSON::Protobuf(label));
  }
}


void json(JSON::ObjectWriter* writer, const MasterInfo& info)
{
  writer->field("id", info.id());
  writer->field("pid", info.pid());
  writer->field("port", info.port());
  writer->field("hostname", info.hostname());

  if (info.has_domain()) {
    writer->field("domain", info.domain());
  }
}


void json(
    JSON::StringWriter* writer,
    const MasterInfo::Capability& capability)
{
  writer->set(MasterInfo::Capability::Type_Name(capability.type()));
}


static void json(JSON::ObjectWriter* writer, const NetworkInfo& info)
{
  if (info.groups().size() > 0) {
    writer->field("groups", info.groups());
  }

  if (info.has_labels()) {
    writer->field("labels", info.labels());
  }

  if (info.ip_addresses().size() > 0) {
    writer->field("ip_addresses", [&info](JSON::ArrayWriter* writer) {
      foreach (const NetworkInfo::IPAddress& ipAddress, info.ip_addresses()) {
        writer->element(JSON::Protobuf(ipAddress));
      }
    });
  }

  if (info.has_name()) {
    writer->field("name", info.name());
  }

  if (info.port_mappings().size() > 0) {
    writer->field("port_mappings", [&info](JSON::ArrayWriter* writer) {
      foreach(const NetworkInfo::PortMapping& portMapping,
              info.port_mappings()) {
        writer->element(JSON::Protobuf(portMapping));
      }
    });
  }
}


void json(JSON::ObjectWriter* writer, const Offer& offer)
{
  writer->field("id", offer.id().value());
  writer->field("framework_id", offer.framework_id().value());
  writer->field("allocation_info", JSON::Protobuf(offer.allocation_info()));
  writer->field("slave_id", offer.slave_id().value());
  writer->field("resources", offer.resources());
}


template <typename ResourceIterable>
void json(
    JSON::ObjectWriter* writer,
    ResourceIterable begin,
    ResourceIterable end)
{
  hashmap<string, double> scalars =
    {{"cpus", 0}, {"gpus", 0}, {"mem", 0}, {"disk", 0}};
  hashmap<string, Value::Ranges> ranges;
  hashmap<string, Value::Set> sets;

  for (auto it = begin; it != end; ++it) {
    const Resource& resource = *it;

    string name =
      resource.name() + (Resources::isRevocable(resource) ? "_revocable" : "");
    switch (resource.type()) {
      case Value::SCALAR:
        scalars[name] += resource.scalar().value();
        break;
      case Value::RANGES:
        ranges[name] += resource.ranges();
        break;
      case Value::SET:
        sets[name] += resource.set();
        break;
      case Value::TEXT:
        break;
    }
  }

  json(writer, scalars);
  json(writer, ranges);
  json(writer, sets);
}


void json(JSON::ObjectWriter* writer, const Resources& resources)
{
  json(writer, resources.begin(), resources.end());
}


void json(
    JSON::ObjectWriter* writer,
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  json(writer, resources.begin(), resources.end());
}


void json(JSON::ObjectWriter* writer, const ResourceQuantities& quantities)
{
  foreachpair (const string& name, const Value::Scalar& scalar, quantities) {
    writer->field(name, scalar.value());
  }
}


void json(JSON::ObjectWriter* writer, const ResourceLimits& limits)
{
  foreachpair (const string& name, const Value::Scalar& scalar, limits) {
    writer->field(name, scalar.value());
  }
}


void json(JSON::ObjectWriter* writer, const SlaveInfo& slaveInfo)
{
  writer->field("id", slaveInfo.id().value());
  writer->field("hostname", slaveInfo.hostname());
  writer->field("port", slaveInfo.port());
  writer->field("attributes", Attributes(slaveInfo.attributes()));

  if (slaveInfo.has_domain()) {
    writer->field("domain", slaveInfo.domain());
  }
}


void json(
    JSON::StringWriter* writer,
    const SlaveInfo::Capability& capability)
{
  writer->set(SlaveInfo::Capability::Type_Name(capability.type()));
}


// Used to include resource limits in JSON output.
void json(
    JSON::ObjectWriter* writer,
    const google::protobuf::Map<string, Value_Scalar>& map)
{
  foreach (auto item, map) {
    writer->field(item.first, item.second.value());
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
  writer->field("resources", task.resources());

  if (!task.limits().empty()) {
    writer->field("limits", task.limits());
  }

  // Tasks are not allowed to mix resources allocated to
  // different roles, see MESOS-6636.
  writer->field("role", task.resources().begin()->allocation_info().role());

  writer->field("statuses", task.statuses());

  if (task.has_user()) {
    writer->field("user", task.user());
  }

  if (task.has_labels()) {
    writer->field("labels", task.labels());
  }

  if (task.has_discovery()) {
    writer->field("discovery", JSON::Protobuf(task.discovery()));
  }

  if (task.has_container()) {
    writer->field("container", JSON::Protobuf(task.container()));
  }

  if (task.has_health_check()) {
    writer->field("health_check", JSON::Protobuf(task.health_check()));
  }
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

  if (status.has_healthy()) {
    writer->field("healthy", status.healthy());
  }
}


static void json(JSON::NumberWriter* writer, const Value::Scalar& scalar)
{
  writer->set(scalar.value());
}


static void json(JSON::StringWriter* writer, const Value::Ranges& ranges)
{
  writer->set(stringify(ranges));
}


static void json(JSON::StringWriter* writer, const Value::Set& set)
{
  writer->set(stringify(set));
}


static void json(JSON::StringWriter* writer, const Value::Text& text)
{
  writer->set(text.value());
}


void json(JSON::ObjectWriter* writer, const Quota& quota)
{
  writer->field("guarantees", quota.guarantees);
  writer->field("limits", quota.limits);
}


Future<Owned<ObjectApprovers>> ObjectApprovers::create(
    const Option<Authorizer*>& authorizer,
    const Option<Principal>& principal,
    std::initializer_list<authorization::Action> actions)
{
  // Ensures there are no repeated elements.
  // Note: `set` is necessary because we relay in the order of the elements
  // while doing `zip` below.
  set<authorization::Action> _actions(actions);

  Option<authorization::Subject> subject =
    authorization::createSubject(principal);

  if (authorizer.isNone()) {
    hashmap<authorization::Action, shared_ptr<const ObjectApprover>> approvers;

    foreach (authorization::Action action, _actions) {
      approvers.put(action, std::make_shared<AcceptingObjectApprover>());
    }

    return Owned<ObjectApprovers>(
        new ObjectApprovers(std::move(approvers), principal));
  }

  return process::collect(lambda::map<vector>(
      [&](authorization::Action action) {
        return authorizer.get()->getApprover(subject, action);
      },
      _actions))
    .then([=](const vector<shared_ptr<const ObjectApprover>>& _approvers) {
      return Owned<ObjectApprovers>(
          new ObjectApprovers(lambda::zip(_actions, _approvers), principal));
    });
}


process::Future<bool> authorizeEndpoint(
    const string& endpoint,
    const string& method,
    const Option<Authorizer*>& authorizer,
    const Option<Principal>& principal)
{
  if (authorizer.isNone()) {
    return true;
  }

  authorization::Request request;

  // TODO(nfnt): Add an additional case when POST requests
  // need to be authorized separately from GET requests.
  if (method == "GET") {
    request.set_action(authorization::GET_ENDPOINT_WITH_PATH);
  } else {
    return Failure("Unexpected request method '" + method + "'");
  }

  if (!authorization::AUTHORIZABLE_ENDPOINTS.contains(endpoint)) {
    return Failure(
        "Endpoint '" + endpoint + "' is not an authorizable endpoint.");
  }

  Option<authorization::Subject> subject =
    authorization::createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  request.mutable_object()->set_value(endpoint);

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to " << method
            << " the '" << endpoint << "' endpoint";

  return authorizer.get()->authorized(request);
}


namespace {

Result<process::http::authentication::Authenticator*> createBasicAuthenticator(
    const string& realm,
    const string& authenticatorName,
    const Option<Credentials>& credentials)
{
  if (credentials.isNone()) {
    return Error(
        "No credentials provided for the default '" +
        string(internal::DEFAULT_BASIC_HTTP_AUTHENTICATOR) +
        "' HTTP authenticator for realm '" + realm + "'");
  }

  LOG(INFO) << "Creating default '"
            << internal::DEFAULT_BASIC_HTTP_AUTHENTICATOR
            << "' HTTP authenticator for realm '" << realm << "'";

  return BasicAuthenticatorFactory::create(realm, credentials.get());
}


#ifdef USE_SSL_SOCKET
Result<process::http::authentication::Authenticator*> createJWTAuthenticator(
    const string& realm,
    const string& authenticatorName,
    const Option<string>& jwtSecretKey)
{
  if (jwtSecretKey.isNone()) {
    return Error(
        "No secret key provided for the default '" +
        string(internal::DEFAULT_JWT_HTTP_AUTHENTICATOR) +
        "' HTTP authenticator for realm '" + realm + "'");
  }

  LOG(INFO) << "Creating default '"
            << internal::DEFAULT_JWT_HTTP_AUTHENTICATOR
            << "' HTTP authenticator for realm '" << realm << "'";

  return new JWTAuthenticator(realm, jwtSecretKey.get());
}
#endif // USE_SSL_SOCKET


Result<process::http::authentication::Authenticator*> createCustomAuthenticator(
    const string& realm,
    const string& authenticatorName)
{
  if (!modules::ModuleManager::contains<
        process::http::authentication::Authenticator>(authenticatorName)) {
    return Error(
        "HTTP authenticator '" + authenticatorName + "' not found. "
        "Check the spelling (compare to '" +
        string(internal::DEFAULT_BASIC_HTTP_AUTHENTICATOR) +
        "') or verify that the authenticator was loaded "
        "successfully (see --modules)");
  }

  LOG(INFO) << "Creating '" << authenticatorName << "' HTTP authenticator "
            << "for realm '" << realm << "'";

  return modules::ModuleManager::create<
      process::http::authentication::Authenticator>(authenticatorName);
}

} // namespace {

Try<Nothing> initializeHttpAuthenticators(
    const string& realm,
    const vector<string>& authenticatorNames,
    const Option<Credentials>& credentials,
    const Option<string>& jwtSecretKey)
{
  if (authenticatorNames.empty()) {
    return Error(
        "No HTTP authenticators specified for realm '" + realm + "'");
  }

  Option<process::http::authentication::Authenticator*> authenticator;

  if (authenticatorNames.size() == 1) {
    Result<process::http::authentication::Authenticator*> authenticator_ =
      None();

    if (authenticatorNames[0] == internal::DEFAULT_BASIC_HTTP_AUTHENTICATOR) {
      authenticator_ =
        createBasicAuthenticator(realm, authenticatorNames[0], credentials);
#ifdef USE_SSL_SOCKET
    } else if (
        authenticatorNames[0] == internal::DEFAULT_JWT_HTTP_AUTHENTICATOR) {
      authenticator_ =
        createJWTAuthenticator(realm, authenticatorNames[0], jwtSecretKey);
#endif // USE_SSL_SOCKET
    } else {
      authenticator_ = createCustomAuthenticator(realm, authenticatorNames[0]);
    }

    if (authenticator_.isError()) {
      return Error(
          "Failed to create HTTP authenticator module '" +
          authenticatorNames[0] + "': " + authenticator_.error());
    }

    CHECK_SOME(authenticator_);
    authenticator = authenticator_.get();
  } else {
    // There are multiple authenticators loaded for this realm,
    // so construct a `CombinedAuthenticator` to handle them.
    vector<Owned<process::http::authentication::Authenticator>> authenticators;
    foreach (const string& name, authenticatorNames) {
      Result<process::http::authentication::Authenticator*> authenticator_ =
        None();

      if (name == internal::DEFAULT_BASIC_HTTP_AUTHENTICATOR) {
        authenticator_ = createBasicAuthenticator(realm, name, credentials);
#ifdef USE_SSL_SOCKET
      } else if (name == internal::DEFAULT_JWT_HTTP_AUTHENTICATOR) {
        authenticator_ = createJWTAuthenticator(realm, name, jwtSecretKey);
#endif // USE_SSL_SOCKET
      } else {
        authenticator_ = createCustomAuthenticator(realm, name);
      }

      if (authenticator_.isError()) {
        return Error(
            "Failed to create HTTP authenticator module '" +
            name + "': " + authenticator_.error());
      }

      CHECK_SOME(authenticator_);
      authenticators.push_back(
          Owned<process::http::authentication::Authenticator>(
              authenticator_.get()));
    }

    authenticator = new CombinedAuthenticator(realm, std::move(authenticators));
  }

  CHECK(authenticator.isSome());

  // Ownership of the authenticator is passed to libprocess.
  process::http::authentication::setAuthenticator(
      realm, Owned<process::http::authentication::Authenticator>(
          authenticator.get()));

  return Nothing();
}


void logRequest(const process::http::Request& request)
{
  Option<string> userAgent = request.headers.get("User-Agent");
  Option<string> forwardedFor = request.headers.get("X-Forwarded-For");

  LOG(INFO) << "HTTP " << request.method << " for " << request.url
            << (request.client.isSome()
                ? " from " + stringify(request.client.get())
                : "")
            << (userAgent.isSome()
                ? " with User-Agent='" + userAgent.get() + "'"
                : "")
            << (forwardedFor.isSome()
                ? " with X-Forwarded-For='" + forwardedFor.get() + "'"
                : "");
}


void logResponse(
    const process::http::Request& request,
    const process::http::Response& response)
{
  LOG(INFO) << "HTTP " << request.method << " for " << request.url
            << (request.client.isSome()
                ? " from " + stringify(request.client.get())
                : "")
            << ": '" << response.status << "'"
            << " after " << (process::Clock::now() - request.received).ms()
            << Milliseconds::units();
}

}  // namespace mesos {
