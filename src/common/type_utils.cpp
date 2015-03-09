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
#include <mesos/type_utils.hpp>

#include "common/attributes.hpp"

#include "messages/messages.hpp"

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

  return left.environment() == right.environment() &&
    left.value() == right.value() &&
    left.shell() == right.shell();
}


bool operator == (const CommandInfo::URI& left, const CommandInfo::URI& right)
{
  return left.executable() == right.executable() &&
    left.value() == right.value();
}


bool operator == (const Credential& left, const Credential& right)
{
  return left.principal() == right.principal() &&
    left.secret() == right.secret();
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
    left.framework_id() == right.framework_id() &&
    left.command() == right.command() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.name() == right.name() &&
    left.source() == right.source() &&
    left.data() == right.data();
}


bool operator == (const MasterInfo& left, const MasterInfo& right)
{
  return left.id() == right.id() &&
    left.ip() == right.ip() &&
    left.port() == right.port() &&
    left.pid() == right.pid() &&
    left.hostname() == right.hostname();
}


bool operator == (const SlaveInfo& left, const SlaveInfo& right)
{
  return left.hostname() == right.hostname() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    internal::Attributes(left.attributes()) ==
      internal::Attributes(right.attributes()) &&
    left.id() == right.id() &&
    left.checkpoint() == right.checkpoint();
}


bool operator == (const Volume& left, const Volume& right)
{
  return left.container_path() == right.container_path() &&
    left.mode() == right.mode() &&
    left.host_path() == right.host_path();
}


namespace internal {

bool operator == (const Task& left, const Task& right)
{
  return left.name() == right.name() &&
    left.task_id() == right.task_id() &&
    left.framework_id() == right.framework_id() &&
    left.slave_id() == right.slave_id() &&
    left.state() == right.state() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.executor_id() == right.executor_id();
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

} // namespace internal {
} // namespace mesos {
