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

#ifndef __MESOS_CSI_V1_HPP__
#define __MESOS_CSI_V1_HPP__

#include <ostream>
#include <type_traits>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <csi/v1/csi.pb.h>

// ONLY USEFUL AFTER RUNNING PROTOC WITH GRPC CPP PLUGIN.
#include <csi/v1/csi.grpc.pb.h>

#include <google/protobuf/message.h>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>

#include <stout/check.hpp>

namespace mesos {
namespace csi {
namespace v1 {

using namespace ::csi::v1;

constexpr char API_VERSION[] = "v1";

} // namespace v1 {
} // namespace csi {
} // namespace mesos {


namespace csi {
namespace v1 {

// Default implementation for comparing protobuf messages in namespace
// `csi::v1`. Note that any non-template overloading of the equality operator
// would take precedence over this function template.
template <
    typename Message,
    typename std::enable_if<std::is_convertible<
        Message*, google::protobuf::Message*>::value, int>::type = 0>
bool operator==(const Message& left, const Message& right)
{
  // NOTE: `MessageDifferencer::Equivalent` would ignore unknown fields and load
  // default values for unset fields (which are indistinguishable in proto3).
  return google::protobuf::util::MessageDifferencer::Equivalent(left, right);
}


template <
    typename Message,
    typename std::enable_if<std::is_convertible<
        Message*, google::protobuf::Message*>::value, int>::type = 0>
bool operator!=(const Message& left, const Message& right)
{
  return !(left == right);
}


std::ostream& operator<<(
    std::ostream& stream,
    const ControllerServiceCapability::RPC::Type& type);


// Default implementation for output protobuf messages in namespace `csi::v1`.
// Note that any non-template overloading of the output operator would take
// precedence over this function template.
template <
    typename Message,
    typename std::enable_if<std::is_convertible<
        Message*, google::protobuf::Message*>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& stream, const Message& message)
{
  // NOTE: We use Google's JSON utility functions for proto3.
  std::string output;
  google::protobuf::util::Status status =
    google::protobuf::util::MessageToJsonString(message, &output);

  CHECK(status.ok())
    << "Could not convert messages to string: " << status.error_message();

  return stream << output;
}

} // namespace v1 {
} // namespace csi {

#endif // __MESOS_CSI_V1_HPP__
