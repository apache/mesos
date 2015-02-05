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

#include <mesos/mesos.hpp>

#include <stout/json.hpp>

#include "messages/messages.hpp"

namespace mesos {

class Attributes;
class Resources;


JSON::Object model(const Resources& resources);
JSON::Object model(const Attributes& attributes);

// These are the two identical ways to model a task, depending on
// whether you have a 'Task' or a 'TaskInfo' available.
JSON::Object model(const Task& task);
JSON::Object model(
    const TaskInfo& task,
    const FrameworkID& frameworkId,
    const TaskState& state,
    const std::vector<TaskStatus>& statuses);

} // namespace mesos {

#endif // __COMMON_HTTP_HPP__
