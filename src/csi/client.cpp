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

#include "csi/client.hpp"

using process::Future;

namespace mesos {
namespace csi {

Future<GetSupportedVersionsResponse> Client::GetSupportedVersions(
    const GetSupportedVersionsRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Identity, GetSupportedVersions),
      request);
}


Future<GetPluginInfoResponse> Client::GetPluginInfo(
    const GetPluginInfoRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Identity, GetPluginInfo),
      request);
}


Future<CreateVolumeResponse> Client::CreateVolume(
    const CreateVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, CreateVolume),
      request);
}


Future<DeleteVolumeResponse> Client::DeleteVolume(
    const DeleteVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, DeleteVolume),
      request);
}


Future<ControllerPublishVolumeResponse> Client::ControllerPublishVolume(
    const ControllerPublishVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, ControllerPublishVolume),
      request);
}


Future<ControllerUnpublishVolumeResponse> Client::ControllerUnpublishVolume(
    const ControllerUnpublishVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, ControllerUnpublishVolume),
      request);
}


Future<ValidateVolumeCapabilitiesResponse> Client::ValidateVolumeCapabilities(
    const ValidateVolumeCapabilitiesRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, ValidateVolumeCapabilities),
      request);
}


Future<ListVolumesResponse> Client::ListVolumes(
    const ListVolumesRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, ListVolumes),
      request);
}


Future<GetCapacityResponse> Client::GetCapacity(
    const GetCapacityRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, GetCapacity),
      request);
}


Future<ControllerGetCapabilitiesResponse> Client::ControllerGetCapabilities(
    const ControllerGetCapabilitiesRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Controller, ControllerGetCapabilities),
      request);
}


Future<NodePublishVolumeResponse> Client::NodePublishVolume(
    const NodePublishVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Node, NodePublishVolume),
      request);
}


Future<NodeUnpublishVolumeResponse> Client::NodeUnpublishVolume(
    const NodeUnpublishVolumeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Node, NodeUnpublishVolume),
      request);
}


Future<GetNodeIDResponse> Client::GetNodeID(
    const GetNodeIDRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Node, GetNodeID),
      request);
}


Future<ProbeNodeResponse> Client::ProbeNode(
    const ProbeNodeRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Node, ProbeNode),
      request);
}


Future<NodeGetCapabilitiesResponse> Client::NodeGetCapabilities(
    const NodeGetCapabilitiesRequest& request)
{
  return runtime.call(
      channel,
      GRPC_RPC(Node, NodeGetCapabilities),
      request);
}

} // namespace csi {
} // namespace mesos {
