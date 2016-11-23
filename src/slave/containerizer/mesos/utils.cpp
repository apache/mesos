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

#include "slave/containerizer/mesos/utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

ContainerID getRootContainerId(const ContainerID& containerId)
{
  ContainerID rootContainerId = containerId;
  while (rootContainerId.has_parent()) {
    // NOTE: Looks like protobuf does not handle copying well when
    // nesting message is involved, because the source and the target
    // point to the same object. Therefore, we create a temporary
    // variable and use an extra copy here.
    ContainerID id = rootContainerId.parent();
    rootContainerId = id;
  }

  return rootContainerId;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
