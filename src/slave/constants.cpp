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

#include "master/constants.hpp"

#include "slave/constants.hpp"

#include <vector>

using std::vector;

namespace mesos {
namespace internal {
namespace slave {

Duration DEFAULT_MASTER_PING_TIMEOUT()
{
  return master::DEFAULT_AGENT_PING_TIMEOUT *
    master::DEFAULT_MAX_AGENT_PING_TIMEOUTS;
}


vector<SlaveInfo::Capability> AGENT_CAPABILITIES()
{
  SlaveInfo::Capability::Type types[] = {
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER,
    SlaveInfo::Capability::RESIZE_VOLUME,
    SlaveInfo::Capability::AGENT_OPERATION_FEEDBACK,
    SlaveInfo::Capability::AGENT_DRAINING,
    SlaveInfo::Capability::TASK_RESOURCE_LIMITS,
  };

  vector<SlaveInfo::Capability> result;
  foreach (SlaveInfo::Capability::Type type, types) {
    SlaveInfo::Capability capability;
    capability.set_type(type);
    result.push_back(capability);
  }

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
