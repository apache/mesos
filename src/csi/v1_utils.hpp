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

#ifndef __CSI_V1_UTILS_HPP__
#define __CSI_V1_UTILS_HPP__

#include <google/protobuf/message.h>

#include <mesos/mesos.hpp>

#include <mesos/csi/v1.hpp>

#include <stout/foreach.hpp>
#include <stout/unreachable.hpp>

namespace mesos {
namespace csi {
namespace v1 {

using CSIVolume = Volume::Source::CSIVolume;


struct PluginCapabilities
{
  PluginCapabilities() = default;

  template <typename Iterable>
  PluginCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_service() &&
          PluginCapability::Service::Type_IsValid(
              capability.service().type())) {
        switch (capability.service().type()) {
          case PluginCapability::Service::UNKNOWN:
            break;
          case PluginCapability::Service::CONTROLLER_SERVICE:
            controllerService = true;
            break;
          case PluginCapability::Service::VOLUME_ACCESSIBILITY_CONSTRAINTS:
            volumeAccessibilityConstraints = true;
            break;

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }

      if (capability.has_volume_expansion() &&
          PluginCapability::VolumeExpansion::Type_IsValid(
              capability.volume_expansion().type())) {
        switch (capability.volume_expansion().type()) {
          case PluginCapability::VolumeExpansion::UNKNOWN:
            break;
          case PluginCapability::VolumeExpansion::ONLINE:
            volumeExpansionOnline = true;
            volumeExpansionOffline = false;
            break;
          case PluginCapability::VolumeExpansion::OFFLINE:
            volumeExpansionOnline = false;
            volumeExpansionOffline = true;
            break;

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool controllerService = false;
  bool volumeAccessibilityConstraints = false;
  bool volumeExpansionOnline = false;
  bool volumeExpansionOffline = false;
};


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
        switch (capability.rpc().type()) {
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
          case ControllerServiceCapability::RPC::CREATE_DELETE_SNAPSHOT:
            createDeleteSnapshot = true;
            break;
          case ControllerServiceCapability::RPC::LIST_SNAPSHOTS:
            listSnapshots = true;
            break;
          case ControllerServiceCapability::RPC::CLONE_VOLUME:
            cloneVolume = true;
            break;
          case ControllerServiceCapability::RPC::PUBLISH_READONLY:
            publishReadonly = true;
            break;
          case ControllerServiceCapability::RPC::EXPAND_VOLUME:
            expandVolume = true;
            break;

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
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
  bool createDeleteSnapshot = false;
  bool listSnapshots = false;
  bool cloneVolume = false;
  bool publishReadonly = false;
  bool expandVolume = false;
};


struct NodeCapabilities
{
  NodeCapabilities() = default;

  template <typename Iterable>
  NodeCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_rpc() &&
          NodeServiceCapability::RPC::Type_IsValid(capability.rpc().type())) {
        switch (capability.rpc().type()) {
          case NodeServiceCapability::RPC::UNKNOWN:
            break;
          case NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME:
            stageUnstageVolume = true;
            break;
          case NodeServiceCapability::RPC::GET_VOLUME_STATS:
            getVolumeStats = true;
            break;
          case NodeServiceCapability::RPC::EXPAND_VOLUME:
            expandVolume = true;
            break;

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool stageUnstageVolume = false;
  bool getVolumeStats = false;
  bool expandVolume = false;
};


// Helpers to devolve CSI v1 protobufs to their unversioned counterparts.
CSIVolume::VolumeCapability devolve(const VolumeCapability& capability);

google::protobuf::RepeatedPtrField<CSIVolume::VolumeCapability> devolve(
    const google::protobuf::RepeatedPtrField<VolumeCapability>& capabilities);


// Helpers to evolve unversioned CSI protobufs to their v1 counterparts.
VolumeCapability evolve(const CSIVolume::VolumeCapability& capability);

google::protobuf::RepeatedPtrField<VolumeCapability> evolve(
    const google::protobuf::RepeatedPtrField<CSIVolume::VolumeCapability>&
      capabilities);

} // namespace v1 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_V1_UTILS_HPP__
