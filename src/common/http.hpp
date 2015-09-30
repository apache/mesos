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

#ifndef __COMMON_HTTP_HPP__
#define __COMMON_HTTP_HPP__

#include <vector>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>

namespace mesos {

class Resources;
class Attributes;

namespace internal {

class Task;


// Serializes a protobuf message for transmission
// based on the HTTP content type.
std::string serialize(
    ContentType contentType,
    const google::protobuf::Message& message);


// Deserializes a string message into a protobuf message based on the
// HTTP content type.
template <typename Message>
Try<Message> deserialize(
    ContentType contentType,
    const std::string& body)
{
  switch (contentType) {
    case ContentType::PROTOBUF: {
      Message message;
      if (!message.ParseFromString(body)) {
        return Error("Failed to parse body into a protobuf object");
      }
      return message;
    }
    case ContentType::JSON: {
      Try<JSON::Value> value = JSON::parse(body);
      if (value.isError()) {
        return Error("Failed to parse body into JSON: " + value.error());
      }

      return ::protobuf::parse<Message>(value.get());
    }
  }

  UNREACHABLE();
}


JSON::Object model(const Resources& resources);
JSON::Object model(const hashmap<std::string, Resources>& roleResources);
JSON::Object model(const Attributes& attributes);
JSON::Object model(const CommandInfo& command);
JSON::Object model(const ExecutorInfo& executorInfo);
JSON::Array model(const Labels& labels);
JSON::Object model(const NetworkInfo& info);
JSON::Object model(const ContainerStatus& status);

// These are the two identical ways to model a task, depending on
// whether you have a 'Task' or a 'TaskInfo' available.
JSON::Object model(const Task& task);
JSON::Object model(
    const TaskInfo& task,
    const FrameworkID& frameworkId,
    const TaskState& state,
    const std::vector<TaskStatus>& statuses);

} // namespace internal {
} // namespace mesos {

#endif // __COMMON_HTTP_HPP__
