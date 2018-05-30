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

#ifndef __CSI_RPC_HPP__
#define __CSI_RPC_HPP__

#include <ostream>

#include <csi/spec.hpp>

namespace mesos {
namespace csi {
namespace v0 {

enum RPC
{
  // RPCs for the Identity service.
  GET_PLUGIN_INFO,
  GET_PLUGIN_CAPABILITIES,
  PROBE,

  // RPCs for the Controller service.
  CREATE_VOLUME,
  DELETE_VOLUME,
  CONTROLLER_PUBLISH_VOLUME,
  CONTROLLER_UNPUBLISH_VOLUME,
  VALIDATE_VOLUME_CAPABILITIES,
  LIST_VOLUMES,
  GET_CAPACITY,
  CONTROLLER_GET_CAPABILITIES,

  // RPCs for the Node service.
  NODE_STAGE_VOLUME,
  NODE_UNSTAGE_VOLUME,
  NODE_PUBLISH_VOLUME,
  NODE_UNPUBLISH_VOLUME,
  NODE_GET_ID,
  NODE_GET_CAPABILITIES
};


template <RPC>
struct RPCTraits;


template <>
struct RPCTraits<GET_PLUGIN_INFO>
{
  typedef GetPluginInfoRequest request_type;
  typedef GetPluginInfoResponse response_type;
};


template <>
struct RPCTraits<GET_PLUGIN_CAPABILITIES>
{
  typedef GetPluginCapabilitiesRequest request_type;
  typedef GetPluginCapabilitiesResponse response_type;
};


template <>
struct RPCTraits<PROBE>
{
  typedef ProbeRequest request_type;
  typedef ProbeResponse response_type;
};


template <>
struct RPCTraits<CREATE_VOLUME>
{
  typedef CreateVolumeRequest request_type;
  typedef CreateVolumeResponse response_type;
};


template <>
struct RPCTraits<DELETE_VOLUME>
{
  typedef DeleteVolumeRequest request_type;
  typedef DeleteVolumeResponse response_type;
};


template <>
struct RPCTraits<CONTROLLER_PUBLISH_VOLUME>
{
  typedef ControllerPublishVolumeRequest request_type;
  typedef ControllerPublishVolumeResponse response_type;
};


template <>
struct RPCTraits<CONTROLLER_UNPUBLISH_VOLUME>
{
  typedef ControllerUnpublishVolumeRequest request_type;
  typedef ControllerUnpublishVolumeResponse response_type;
};


template <>
struct RPCTraits<VALIDATE_VOLUME_CAPABILITIES>
{
  typedef ValidateVolumeCapabilitiesRequest request_type;
  typedef ValidateVolumeCapabilitiesResponse response_type;
};


template <>
struct RPCTraits<LIST_VOLUMES>
{
  typedef ListVolumesRequest request_type;
  typedef ListVolumesResponse response_type;
};


template <>
struct RPCTraits<GET_CAPACITY>
{
  typedef GetCapacityRequest request_type;
  typedef GetCapacityResponse response_type;
};


template <>
struct RPCTraits<CONTROLLER_GET_CAPABILITIES>
{
  typedef ControllerGetCapabilitiesRequest request_type;
  typedef ControllerGetCapabilitiesResponse response_type;
};


template <>
struct RPCTraits<NODE_STAGE_VOLUME>
{
  typedef NodeStageVolumeRequest request_type;
  typedef NodeStageVolumeResponse response_type;
};


template <>
struct RPCTraits<NODE_UNSTAGE_VOLUME>
{
  typedef NodeUnstageVolumeRequest request_type;
  typedef NodeUnstageVolumeResponse response_type;
};


template <>
struct RPCTraits<NODE_PUBLISH_VOLUME>
{
  typedef NodePublishVolumeRequest request_type;
  typedef NodePublishVolumeResponse response_type;
};


template <>
struct RPCTraits<NODE_UNPUBLISH_VOLUME>
{
  typedef NodeUnpublishVolumeRequest request_type;
  typedef NodeUnpublishVolumeResponse response_type;
};


template <>
struct RPCTraits<NODE_GET_ID>
{
  typedef NodeGetIdRequest request_type;
  typedef NodeGetIdResponse response_type;
};


template <>
struct RPCTraits<NODE_GET_CAPABILITIES>
{
  typedef NodeGetCapabilitiesRequest request_type;
  typedef NodeGetCapabilitiesResponse response_type;
};


std::ostream& operator<<(std::ostream& stream, const RPC& rpc);

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_RPC_HPP__
