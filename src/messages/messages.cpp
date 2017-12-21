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

#include "messages/messages.hpp"

#include <ostream>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

using std::ostream;

namespace mesos {
namespace internal {

bool operator==(
    const ResourceVersionUUID& left,
    const ResourceVersionUUID& right)
{
  if (left.has_resource_provider_id() != right.has_resource_provider_id()) {
    return false;
  }

  if (left.has_resource_provider_id() &&
      left.resource_provider_id() != right.resource_provider_id()) {
    return false;
  }

  if (left.uuid() != right.uuid()) {
    return false;
  }

  return true;
}


bool operator!=(
    const ResourceVersionUUID& left,
    const ResourceVersionUUID& right)
{
  return !(left == right);
}


bool operator==(
    const UpdateOperationStatusMessage& left,
    const UpdateOperationStatusMessage& right)
{
  if (left.has_framework_id() != right.has_framework_id()) {
    return false;
  }

  if (left.has_framework_id() && left.framework_id() != right.framework_id()) {
    return false;
  }

  if (left.has_slave_id() != right.has_slave_id()) {
    return false;
  }

  if (left.has_slave_id() && left.slave_id() != right.slave_id()) {
    return false;
  }

  if (left.status() != right.status()) {
    return false;
  }

  if (left.has_latest_status() != right.has_latest_status()) {
    return false;
  }

  if (left.has_latest_status() &&
      left.latest_status() != right.latest_status()) {
    return false;
  }

  if (left.operation_uuid() != right.operation_uuid()) {
    return false;
  }

  return true;
}


bool operator!=(
    const UpdateOperationStatusMessage& left,
    const UpdateOperationStatusMessage& right)
{
  return !(left == right);
}


ostream& operator<<(ostream& stream, const StatusUpdate& update)
{
  stream << update.status().state();

  if (update.has_uuid()) {
    stream << " (Status UUID: "
           << stringify(id::UUID::fromBytes(update.uuid()).get()) << ")";
  }

  stream << " for task " << update.status().task_id();

  if (update.status().has_healthy()) {
    stream << " in health state "
           << (update.status().healthy() ? "healthy" : "unhealthy");
  }

  return stream << " of framework " << update.framework_id();
}


ostream& operator<<(ostream& stream, const UpdateOperationStatusMessage& update)
{
  stream << update.status().state();

  if (update.status().has_uuid()) {
    stream << " (Status UUID: " << stringify(update.status().uuid()) << ")";
  }

  stream << " for operation UUID " << stringify(update.operation_uuid());

  if (update.status().has_operation_id()) {
    stream << " (framework-supplied ID '" << update.status().operation_id()
           << "')";
  }

  if (update.has_framework_id()) {
    stream << " of framework '" << update.framework_id() << "'";
  }

  if (update.has_slave_id()) {
    stream << " on agent " << update.slave_id();
  }

  return stream;
}


ostream& operator<<(ostream& stream, const StatusUpdateRecord::Type& type)
{
  return stream
    << StatusUpdateRecord::Type_descriptor()->FindValueByNumber(type)->name();
}


ostream& operator<<(
    ostream& stream,
    const UpdateOperationStatusRecord::Type& type)
{
  return stream << UpdateOperationStatusRecord::Type_descriptor()
                     ->FindValueByNumber(type)
                     ->name();
}

} // namespace internal {
} // namespace mesos {
