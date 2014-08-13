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

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include "common/attributes.hpp"
#include "common/type_utils.hpp"

using std::string;

namespace mesos {

static bool equals(
    const google::protobuf::Message& left,
    const google::protobuf::Message& right)
{
  string _left;
  string _right;

  // NOTE: If either of the two messages is not initialized, we will
  // treat them as not equal.
  if (!left.SerializeToString(&_left)) {
    return false;
  }

  if (!right.SerializeToString(&_right)) {
    return false;
  }

  return _left == _right;
}


bool operator == (const Environment& left, const Environment& right)
{
  return equals(left, right);
}


bool operator == (const CommandInfo& left, const CommandInfo& right)
{
  return equals(left, right);
}


bool operator == (const ExecutorInfo& left, const ExecutorInfo& right)
{
  return equals(left, right);
}


bool operator == (const SlaveInfo& left, const SlaveInfo& right)
{
  // NOTE: We don't compare 'webui_hostname' and 'webui_port' since
  // they're deprecated and do not carry any semantic meaning.
  return left.hostname() == right.hostname() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    internal::Attributes(left.attributes()) ==
    internal::Attributes(right.attributes()) &&
    left.has_id() == right.has_id() &&
    (!left.has_id() || (left.id() == right.id())) &&
    left.has_checkpoint() == right.has_checkpoint() &&
    (!left.has_checkpoint() || (left.checkpoint() == right.checkpoint()));
}


bool operator == (const MasterInfo& left, const MasterInfo& right)
{
  return equals(left, right);
}


namespace internal {

bool operator == (const Task& left, const Task& right)
{
  return equals(left, right);
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
