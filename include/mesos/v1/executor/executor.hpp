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

#ifndef __MESOS_V1_EXECUTOR_PROTO_HPP__
#define __MESOS_V1_EXECUTOR_PROTO_HPP__

// NOTE: This header only becomes valid after running protoc
// and generating the equivalent .ph.h files
#include <mesos/v1/executor/executor.pb.h>

namespace mesos {
namespace v1 {
namespace executor {

inline std::ostream& operator<<(std::ostream& stream, const Call::Type& type)
{
  return stream << Call::Type_Name(type);
}


inline std::ostream& operator<<(std::ostream& stream, const Event::Type& type)
{
  return stream << Event::Type_Name(type);
}

} // namespace executor {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_EXECUTOR_PROTO_HPP__
