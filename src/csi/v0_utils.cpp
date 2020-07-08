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

#include "csi/v0_utils.hpp"

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace csi {
namespace v0 {

// Helper for repeated field devolving to `T1` from `T2`.
template <typename T1, typename T2>
RepeatedPtrField<T1> devolve(RepeatedPtrField<T2> from)
{
  RepeatedPtrField<T1> to;
  foreach (const T2& value, from) {
    *to.Add() = devolve(value);
  }

  return to;
}


CSIVolume::VolumeCapability::BlockVolume devolve(
    const VolumeCapability::BlockVolume& block)
{
  return CSIVolume::VolumeCapability::BlockVolume();
}


CSIVolume::VolumeCapability::MountVolume devolve(
    const VolumeCapability::MountVolume& mount)
{
  CSIVolume::VolumeCapability::MountVolume result;
  result.set_fs_type(mount.fs_type());
  *result.mutable_mount_flags() = mount.mount_flags();
  return result;
}


CSIVolume::VolumeCapability::AccessMode devolve(
    const VolumeCapability::AccessMode& accessMode)
{
  CSIVolume::VolumeCapability::AccessMode result;

  switch (accessMode.mode()) {
    case VolumeCapability::AccessMode::UNKNOWN: {
      result.set_mode(CSIVolume::VolumeCapability::AccessMode::UNKNOWN);
      break;
    }
    case VolumeCapability::AccessMode::SINGLE_NODE_WRITER: {
      result.set_mode(
          CSIVolume::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);
      break;
    }
    case VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY: {
      result.set_mode(
          CSIVolume::VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY: {
      result.set_mode(
          CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER: {
      result.set_mode(
          CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER);
      break;
    }
    case VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER: {
      result.set_mode(
          CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER);
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


CSIVolume::VolumeCapability devolve(const VolumeCapability& capability)
{
  CSIVolume::VolumeCapability result;

  switch (capability.access_type_case()) {
    case VolumeCapability::kBlock: {
      *result.mutable_block() = devolve(capability.block());
      break;
    }
    case VolumeCapability::kMount: {
      *result.mutable_mount() = devolve(capability.mount());
      break;
    }
    case VolumeCapability::ACCESS_TYPE_NOT_SET: {
      break;
    }
  }

  if (capability.has_access_mode()) {
    *result.mutable_access_mode() = devolve(capability.access_mode());
  }

  return result;
}


RepeatedPtrField<CSIVolume::VolumeCapability> devolve(
    const RepeatedPtrField<VolumeCapability>& capabilities)
{
  return devolve<CSIVolume::VolumeCapability>(capabilities);
}


// Helper for repeated field evolving to `T1` from `T2`.
template <typename T1, typename T2>
RepeatedPtrField<T1> evolve(RepeatedPtrField<T2> from)
{
  RepeatedPtrField<T1> to;
  foreach (const T2& value, from) {
    *to.Add() = evolve(value);
  }

  return to;
}


VolumeCapability::BlockVolume evolve(
    const CSIVolume::VolumeCapability::BlockVolume& block)
{
  return VolumeCapability::BlockVolume();
}


VolumeCapability::MountVolume evolve(
    const CSIVolume::VolumeCapability::MountVolume& mount)
{
  VolumeCapability::MountVolume result;
  result.set_fs_type(mount.fs_type());
  *result.mutable_mount_flags() = mount.mount_flags();
  return result;
}


VolumeCapability::AccessMode evolve(
    const CSIVolume::VolumeCapability::AccessMode& accessMode)
{
  VolumeCapability::AccessMode result;

  switch (accessMode.mode()) {
    case CSIVolume::VolumeCapability::AccessMode::UNKNOWN: {
      result.set_mode(VolumeCapability::AccessMode::UNKNOWN);
      break;
    }
    case CSIVolume::VolumeCapability::AccessMode::SINGLE_NODE_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::SINGLE_NODE_WRITER);
      break;
    }
    case CSIVolume::VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY: {
      result.set_mode(VolumeCapability::AccessMode::SINGLE_NODE_READER_ONLY);
      break;
    }
    case CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_READER_ONLY);
      break;
    }
    case CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER);
      break;
    }
    case CSIVolume::VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER: {
      result.set_mode(VolumeCapability::AccessMode::MULTI_NODE_MULTI_WRITER);
      break;
    }
  }

  return result;
}


VolumeCapability evolve(const CSIVolume::VolumeCapability& capability)
{
  VolumeCapability result;

  switch (capability.access_type_case()) {
    case CSIVolume::VolumeCapability::kBlock: {
      *result.mutable_block() = evolve(capability.block());
      break;
    }
    case CSIVolume::VolumeCapability::kMount: {
      *result.mutable_mount() = evolve(capability.mount());
      break;
    }
    case CSIVolume::VolumeCapability::ACCESS_TYPE_NOT_SET: {
      break;
    }
  }

  if (capability.has_access_mode()) {
    *result.mutable_access_mode() = evolve(capability.access_mode());
  }

  return result;
}


RepeatedPtrField<VolumeCapability> devolve(
    const RepeatedPtrField<CSIVolume::VolumeCapability>& capabilities)
{
  return evolve<VolumeCapability>(capabilities);
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
