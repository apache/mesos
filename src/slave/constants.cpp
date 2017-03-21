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

#include <vector>

#include "master/constants.hpp"

#include "slave/constants.hpp"

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
  SlaveInfo::Capability multiRoleCapability;
  multiRoleCapability.set_type(SlaveInfo::Capability::MULTI_ROLE);

  return {multiRoleCapability};
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
