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
namespace v0 {

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


bool operator==(const VolumeCapability& left, const VolumeCapability& right) {
  // NOTE: This enumeration is set when `block` or `mount` are set and
  // covers the case where neither are set.
  if (left.access_type_case() != right.access_type_case()) {
    return false;
  }

  // NOTE: No need to check `block` for equality as that object is empty.

  if (left.has_mount()) {
    if (left.mount().fs_type() != right.mount().fs_type()) {
      return false;
    }

    if (left.mount().mount_flags_size() != right.mount().mount_flags_size()) {
      return false;
    }

    // NOTE: Ordering may or may not matter for these flags, but this helper
    // only checks for complete equality.
    for (int i = 0; i < left.mount().mount_flags_size(); i++) {
      if (left.mount().mount_flags(i) != right.mount().mount_flags(i)) {
        return false;
      }
    }
  }

  if (left.has_access_mode() != right.has_access_mode()) {
    return false;
  }

  if (left.has_access_mode()) {
    if (left.access_mode().mode() != right.access_mode().mode()) {
      return false;
    }
  }

  return true;
}


ostream& operator<<(
    ostream& stream,
    const ControllerServiceCapability::RPC::Type& type)
{
  return stream << ControllerServiceCapability::RPC::Type_Name(type);
}

} // namespace v0 {
} // namespace csi {
