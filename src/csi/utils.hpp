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

#ifndef __CSI_UTILS_HPP__
#define __CSI_UTILS_HPP__

#include <ostream>
#include <type_traits>

#include <google/protobuf/map.h>

#include <google/protobuf/util/json_util.h>

#include <mesos/mesos.hpp>

#include <stout/foreach.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "csi/spec.hpp"
#include "csi/state.hpp"

namespace csi {

bool operator==(
    const ControllerServiceCapability& left,
    const ControllerServiceCapability& right);


bool operator==(const Version& left, const Version& right);


bool operator!=(const Version& left, const Version& right);


std::ostream& operator<<(
    std::ostream& stream,
    const ControllerServiceCapability::RPC::Type& type);


std::ostream& operator<<(std::ostream& stream, const Version& version);


// Default imprementation for output protobuf messages in namespace
// `csi`. Note that any non-template overloading of the output operator
// would take precedence over this function template.
template <
    typename Message,
    typename std::enable_if<std::is_convertible<
        Message*, google::protobuf::Message*>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& stream, const Message& message)
{
  // NOTE: We use Google's JSON utility functions for proto3.
  std::string output;
  google::protobuf::util::MessageToJsonString(message, &output);
  return stream << output;
}

} // namespace csi {


namespace mesos {
namespace csi {

struct ControllerCapabilities
{
  ControllerCapabilities() = default;

  template <typename Iterable>
  ControllerCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_rpc() &&
          ControllerServiceCapability::RPC::Type_IsValid(
              capability.rpc().type())) {
        switch(capability.rpc().type()) {
          case ControllerServiceCapability::RPC::UNKNOWN:
            break;
          case ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME:
            createDeleteVolume = true;
            break;
          case ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME:
            publishUnpublishVolume = true;
            break;
          case ControllerServiceCapability::RPC::LIST_VOLUMES:
            listVolumes = true;
            break;
          case ControllerServiceCapability::RPC::GET_CAPACITY:
            getCapacity = true;
            break;
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool createDeleteVolume = false;
  bool publishUnpublishVolume = false;
  bool listVolumes = false;
  bool getCapacity = false;
};


namespace state {

std::ostream& operator<<(std::ostream& stream, const VolumeState::State& state);

} // namespace state {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_UTILS_HPP__
