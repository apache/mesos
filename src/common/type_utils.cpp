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

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include "common/attributes.hpp"
#include "common/type_utils.hpp"

namespace mesos {

bool operator == (const CommandInfo& left, const CommandInfo& right)
{
  if (left.uris().size() != right.uris().size()) {
    return false;
  }

  for (int i = 0; i < left.uris().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.uris().size(); j++) {
      if (left.uris().Get(i) == right.uris().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  if (left.arguments().size() != right.arguments().size()) {
    return false;
  }

  // The order of argv is important.
  for (int i = 0; i < left.arguments().size(); i++) {
    if (left.arguments().Get(i) != right.arguments().Get(i)) {
      return false;
    }
  }

  return left.has_environment() == right.has_environment() &&
    (!left.has_environment() || (left.environment() == right.environment())) &&
    left.has_value() == right.has_value() &&
    (!left.has_value() || (left.value() == right.value())) &&
    left.has_shell() == right.has_shell() &&
    (!left.has_shell() || (left.shell() == right.shell()));
}


bool operator == (const CommandInfo::URI& left, const CommandInfo::URI& right)
{
  return left.has_executable() == right.has_executable() &&
    (!left.has_executable() || (left.executable() == right.executable())) &&
    left.value() == right.value();
}


bool operator == (const Credential& left, const Credential& right)
{
  return left.principal() == right.principal() &&
         left.has_secret() == right.has_secret() &&
         (!left.has_secret() || (left.secret() == right.secret()));
}


bool operator == (const Environment& left, const Environment& right)
{
  if (left.variables().size() != right.variables().size()) {
    return false;
  }

  for (int i = 0; i < left.variables().size(); i++) {
    const std::string& name = left.variables().Get(i).name();
    const std::string& value = left.variables().Get(i).value();
    bool found = false;
    for (int j = 0; j < right.variables().size(); j++) {
      if (name == right.variables().Get(j).name() &&
          value == right.variables().Get(j).value()) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return true;
}


bool operator == (const ExecutorInfo& left, const ExecutorInfo& right)
{
  return left.executor_id() == right.executor_id() &&
    left.has_framework_id() == right.has_framework_id() &&
    (!left.has_framework_id() ||
    (left.framework_id() == right.framework_id())) &&
    left.command() == right.command() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.has_name() == right.has_name() &&
    (!left.has_name() || (left.name() == right.name())) &&
    left.has_source() == right.has_source() &&
    (!left.has_source() || (left.source() == right.source())) &&
    left.has_data() == right.has_data() &&
    (!left.has_data() || (left.data() == right.data()));
}


bool operator == (const MasterInfo& left, const MasterInfo& right)
{
  return left.id() == right.id() &&
    left.ip() == right.ip() &&
    left.port() == right.port() &&
    left.has_pid() == right.has_pid() &&
    (!left.has_pid() || (left.pid() == right.pid())) &&
    left.has_hostname() == right.has_hostname() &&
    (!left.has_hostname() || (left.hostname() == right.hostname()));
}


bool operator == (const SlaveInfo& left, const SlaveInfo& right)
{
  return left.hostname() == right.hostname() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    Attributes(left.attributes()) == Attributes(right.attributes()) &&
    left.has_id() == right.has_id() &&
    (!left.has_id() || (left.id() == right.id())) &&
    left.has_checkpoint() == right.has_checkpoint() &&
    (!left.has_checkpoint() || (left.checkpoint() == right.checkpoint()));
}


bool operator == (const Volume& left, const Volume& right)
{
  return left.container_path() == right.container_path() &&
    left.mode() == right.mode() &&
    left.has_host_path() == right.has_host_path() &&
    (!left.has_host_path() || (left.host_path() == right.host_path()));
}


bool operator == (const Task& left, const Task& right)
{
  return left.name() == right.name() &&
    left.task_id() == right.task_id() &&
    left.framework_id() == right.framework_id() &&
    left.slave_id() == right.slave_id() &&
    left.state() == right.state() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.has_executor_id() == right.has_executor_id() &&
    (!left.has_executor_id() || (left.executor_id() == right.executor_id()));
}


std::ostream& operator << (
    std::ostream& stream,
    const StatusUpdate& update)
{
  stream
    << update.status().state()
    << " (UUID: " << UUID::fromBytes(update.uuid())
    << ") for task " << update.status().task_id();

  if (update.status().has_healthy()) {
    stream
      << " in health state "
      << (update.status().healthy() ? "healthy" : "unhealthy");
  }

  return stream
    << " of framework " << update.framework_id();
}

} // namespace mesos {
