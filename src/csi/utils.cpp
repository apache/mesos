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

#include <stout/unreachable.hpp>

using std::ostream;

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


bool operator==(const VolumeCapability& left, const VolumeCapability& right)
{
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


namespace mesos {
namespace csi {
namespace v0 {

types::VolumeCapability::BlockVolume devolve(
    const VolumeCapability::BlockVolume& blockVolume)
{
  return types::VolumeCapability::BlockVolume();
}


types::VolumeCapability::MountVolume devolve(
    const VolumeCapability::MountVolume& mountVolume)
{
  types::VolumeCapability::MountVolume result;
  result.set_fs_type(mountVolume.fs_type());
  *result.mutable_mount_flags() = mountVolume.mount_flags();
  return result;
}


types::VolumeCapability::AccessMode devolve(
    const VolumeCapability::AccessMode& accessMode)
{
  types::VolumeCapability::AccessMode result;

  switch (accessMode.mode()) {
    case VolumeCapability::AccessMode::UNKNOWN: {
      result.set_mode(types::VolumeCapability::AccessMode::UNKNOWN);
      break;
    }
    case VolumeCapability::AccessMode::SINGLE_NODE_WRITER: {
      result.set_mode(types::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);
      break;
    }
    case VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY: {
      result.set_mode(
          types::VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY: {
      result.set_mode(
          types::VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER: {
      result.set_mode(
          types::VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER: {
      result.set_mode(
          types::VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER);
      break;
    }
    // NOTE: We avoid using a default clause for the following values in
    // proto3's open enum to enable the compiler to detect missing enum cases
    // for us. See: https://github.com/google/protobuf/issues/3917
    case google::protobuf::kint32min:
    case google::protobuf::kint32max: {
      UNREACHABLE();
    }
  }

  return result;
}


types::VolumeCapability devolve(const VolumeCapability& volumeCapability)
{
  types::VolumeCapability result;

  switch (volumeCapability.access_type_case()) {
    case VolumeCapability::kBlock: {
      *result.mutable_block() = devolve(volumeCapability.block());
      break;
    }
    case VolumeCapability::kMount: {
      *result.mutable_mount() = devolve(volumeCapability.mount());
      break;
    }
    case VolumeCapability::ACCESS_TYPE_NOT_SET: {
      break;
    }
  }

  if (volumeCapability.has_access_mode()) {
    *result.mutable_access_mode() = devolve(volumeCapability.access_mode());
  }

  return result;
}


VolumeCapability::BlockVolume evolve(
    const types::VolumeCapability::BlockVolume& blockVolume)
{
  return VolumeCapability::BlockVolume();
}


VolumeCapability::MountVolume evolve(
    const types::VolumeCapability::MountVolume& mountVolume)
{
  VolumeCapability::MountVolume result;
  result.set_fs_type(mountVolume.fs_type());
  *result.mutable_mount_flags() = mountVolume.mount_flags();
  return result;
}


VolumeCapability::AccessMode evolve(
    const types::VolumeCapability::AccessMode& accessMode)
{
  VolumeCapability::AccessMode result;

  switch (accessMode.mode()) {
    case types::VolumeCapability::AccessMode::UNKNOWN: {
      result.set_mode(VolumeCapability::AccessMode::UNKNOWN);
      break;
    }
    case types::VolumeCapability::AccessMode::SINGLE_NODE_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::SINGLE_NODE_WRITER);
      break;
    }
    case types::VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY: {
      result.set_mode(VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY);
      break;
    }
    case types::VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY);
      break;
    }
    case types::VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER);
      break;
    }
    case types::VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER);
      break;
    }
    // NOTE: We avoid using a default clause for the following values in
    // proto3's open enum to enable the compiler to detect missing enum cases
    // for us. See: https://github.com/google/protobuf/issues/3917
    case google::protobuf::kint32min:
    case google::protobuf::kint32max: {
      UNREACHABLE();
    }
  }

  return result;
}


VolumeCapability evolve(const types::VolumeCapability& volumeCapability)
{
  VolumeCapability result;

  switch (volumeCapability.access_type_case()) {
    case types::VolumeCapability::kBlock: {
      *result.mutable_block() = evolve(volumeCapability.block());
      break;
    }
    case types::VolumeCapability::kMount: {
      *result.mutable_mount() = evolve(volumeCapability.mount());
      break;
    }
    case types::VolumeCapability::ACCESS_TYPE_NOT_SET: {
      break;
    }
  }

  if (volumeCapability.has_access_mode()) {
    *result.mutable_access_mode() = evolve(volumeCapability.access_mode());
  }

  return result;
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
