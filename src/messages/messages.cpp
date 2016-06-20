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

ostream& operator<<(ostream& stream, const StatusUpdate& update)
{
  stream << update.status().state();

  if (update.has_uuid()) {
    stream << " (UUID: " << stringify(UUID::fromBytes(update.uuid()).get())
           << ")";
  }

  stream << " for task " << update.status().task_id();

  if (update.status().has_healthy()) {
    stream << " in health state "
           << (update.status().healthy() ? "healthy" : "unhealthy");
  }

  return stream << " of framework " << update.framework_id();
}


ostream& operator<<(ostream& stream, const StatusUpdateRecord::Type& type)
{
  return stream
    << StatusUpdateRecord::Type_descriptor()->FindValueByNumber(type)->name();
}

} // namespace internal {
} // namespace mesos {
