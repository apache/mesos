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


#ifndef __RESOURCE_PROVIDER_REGISTRY_HPP__
#define __RESOURCE_PROVIDER_REGISTRY_HPP__

#include <mesos/type_utils.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "resource_provider/registry.pb.h"

namespace mesos {
namespace resource_provider {
namespace registry {

inline bool operator==(
    const ResourceProvider& left,
    const ResourceProvider& right)
{
  // To support additions to the persisted types we consider two resource
  // providers to be equal if all their set fields are equal.
  if (left.id() != right.id()) {
    return false;
  }

  if (left.has_name() && right.has_name() && left.name() != right.name()) {
    return false;
  }

  if (left.has_type() && right.has_type() && left.type() != right.type()) {
    return false;
  }

  return true;
}


inline bool operator!=(
    const ResourceProvider& left,
    const ResourceProvider& right)
{
  return !(left == right);
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const ResourceProvider& resourceProvider)
{
  return stream << resourceProvider.DebugString();
}

} // namespace registry {
} // namespace resource_provider {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_REGISTRY_HPP__
