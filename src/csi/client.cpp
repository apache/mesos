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

using process::Failure;
using process::Future;

using process::grpc::RpcResult;

namespace mesos {
namespace csi {

Future<GetSupportedVersionsResponse> Client::GetSupportedVersions(
    const GetSupportedVersionsRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Identity, GetSupportedVersions), request)
    .then([](const RpcResult<GetSupportedVersionsResponse>& result)
        -> Future<GetSupportedVersionsResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<GetPluginInfoResponse> Client::GetPluginInfo(
    const GetPluginInfoRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Identity, GetPluginInfo), request)
    .then([](const RpcResult<GetPluginInfoResponse>& result)
        -> Future<GetPluginInfoResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<CreateVolumeResponse> Client::CreateVolume(
    const CreateVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, CreateVolume), request)
    .then([](const RpcResult<CreateVolumeResponse>& result)
        -> Future<CreateVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<DeleteVolumeResponse> Client::DeleteVolume(
    const DeleteVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, DeleteVolume), request)
    .then([](const RpcResult<DeleteVolumeResponse>& result)
        -> Future<DeleteVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ControllerPublishVolumeResponse> Client::ControllerPublishVolume(
    const ControllerPublishVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ControllerPublishVolume), request)
    .then([](const RpcResult<ControllerPublishVolumeResponse>& result)
        -> Future<ControllerPublishVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ControllerUnpublishVolumeResponse> Client::ControllerUnpublishVolume(
    const ControllerUnpublishVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ControllerUnpublishVolume), request)
    .then([](const RpcResult<ControllerUnpublishVolumeResponse>& result)
        -> Future<ControllerUnpublishVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ValidateVolumeCapabilitiesResponse> Client::ValidateVolumeCapabilities(
    const ValidateVolumeCapabilitiesRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ValidateVolumeCapabilities), request)
    .then([](const RpcResult<ValidateVolumeCapabilitiesResponse>& result)
        -> Future<ValidateVolumeCapabilitiesResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ListVolumesResponse> Client::ListVolumes(
    const ListVolumesRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ListVolumes), request)
    .then([](const RpcResult<ListVolumesResponse>& result)
        -> Future<ListVolumesResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<GetCapacityResponse> Client::GetCapacity(
    const GetCapacityRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, GetCapacity), request)
    .then([](const RpcResult<GetCapacityResponse>& result)
        -> Future<GetCapacityResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ControllerProbeResponse> Client::ControllerProbe(
    const ControllerProbeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ControllerProbe), request)
    .then([](const RpcResult<ControllerProbeResponse>& result)
        -> Future<ControllerProbeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<ControllerGetCapabilitiesResponse> Client::ControllerGetCapabilities(
    const ControllerGetCapabilitiesRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Controller, ControllerGetCapabilities), request)
    .then([](const RpcResult<ControllerGetCapabilitiesResponse>& result)
        -> Future<ControllerGetCapabilitiesResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<NodePublishVolumeResponse> Client::NodePublishVolume(
    const NodePublishVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Node, NodePublishVolume), request)
    .then([](const RpcResult<NodePublishVolumeResponse>& result)
        -> Future<NodePublishVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<NodeUnpublishVolumeResponse> Client::NodeUnpublishVolume(
    const NodeUnpublishVolumeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Node, NodeUnpublishVolume), request)
    .then([](const RpcResult<NodeUnpublishVolumeResponse>& result)
        -> Future<NodeUnpublishVolumeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<GetNodeIDResponse> Client::GetNodeID(
    const GetNodeIDRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Node, GetNodeID), request)
    .then([](const RpcResult<GetNodeIDResponse>& result)
        -> Future<GetNodeIDResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<NodeProbeResponse> Client::NodeProbe(
    const NodeProbeRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Node, NodeProbe), request)
    .then([](const RpcResult<NodeProbeResponse>& result)
        -> Future<NodeProbeResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}


Future<NodeGetCapabilitiesResponse> Client::NodeGetCapabilities(
    const NodeGetCapabilitiesRequest& request)
{
  return runtime
    .call(channel, GRPC_RPC(Node, NodeGetCapabilities), request)
    .then([](const RpcResult<NodeGetCapabilitiesResponse>& result)
        -> Future<NodeGetCapabilitiesResponse> {
      if (result.status.ok()) {
        return result.response;
      } else {
        return Failure(result.status.error_message());
      }
    });
}

} // namespace csi {
} // namespace mesos {
