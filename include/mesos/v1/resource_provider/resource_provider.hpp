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

#ifndef __MESOS_V1_RESOURCE_PROVIDER_PROTO_HPP__
#define __MESOS_V1_RESOURCE_PROVIDER_PROTO_HPP__

#include <ostream>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/v1/resource_provider/resource_provider.pb.h>

namespace mesos {
namespace v1 {
namespace resource_provider {

inline std::ostream& operator<<(
    std::ostream& stream,
    const Call::UpdatePublishResourcesStatus::Status& status)
{
  return stream << Call::UpdatePublishResourcesStatus::Status_Name(status);
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const Event::Type& eventType)
{
  return stream << Event::Type_Name(eventType);
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const Call::Type& callType)
{
  return stream << Call::Type_Name(callType);
}

} // namespace resource_provider {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_RESOURCE_PROVIDER_PROTO_HPP__
