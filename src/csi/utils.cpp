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

#include "csi/utils.hpp"

#include <google/protobuf/util/json_util.h>

#include <stout/strings.hpp>

using std::ostream;
using std::string;

using google::protobuf::util::MessageToJsonString;

namespace csi {

bool operator==(
    const ControllerServiceCapability::RPC& left,
    const ControllerServiceCapability::RPC& right)
{
  return left.type() == right.type();
}


bool operator==(
    const ControllerServiceCapability& left,
    const ControllerServiceCapability& right)
{
  return left.has_rpc() == right.has_rpc() &&
    (!left.has_rpc() || left.rpc() == right.rpc());
}


bool operator==(const Version& left, const Version& right)
{
  return left.major() == right.major() &&
    left.minor() == right.minor() &&
    left.patch() == right.patch();
}


bool operator!=(const Version& left, const Version& right)
{
  return !(left == right);
}


ostream& operator<<(
    ostream& stream,
    const ControllerServiceCapability::RPC::Type& type)
{
  return stream << ControllerServiceCapability::RPC::Type_Name(type);
}


ostream& operator<<(ostream& stream, const Version& version)
{
  return stream << strings::join(
      ".",
      version.major(),
      version.minor(),
      version.patch());
}

} // namespace csi {


namespace mesos {
namespace csi {
namespace state {

ostream& operator<<(ostream& stream, const VolumeState::State& state)
{
  return stream << VolumeState::State_Name(state);
}

} // namespace state {
} // namespace csi {
} // namespace mesos {
