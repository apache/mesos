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

#include "csi/v0_client.hpp"

#include <utility>

using process::Future;

using process::grpc::client::CallOptions;

namespace mesos {
namespace csi {
namespace v0 {

Future<RPCResult<GetPluginInfoResponse>>
Client::getPluginInfo(GetPluginInfoRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, GetPluginInfo),
      std::move(request),
      CallOptions());
}


Future<RPCResult<GetPluginCapabilitiesResponse>>
Client::getPluginCapabilities(GetPluginCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, GetPluginCapabilities),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ProbeResponse>> Client::probe(ProbeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, Probe),
      std::move(request),
      CallOptions());
}


Future<RPCResult<CreateVolumeResponse>>
Client::createVolume(CreateVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, CreateVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<DeleteVolumeResponse>>
Client::deleteVolume(DeleteVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, DeleteVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ControllerPublishVolumeResponse>>
Client::controllerPublishVolume(ControllerPublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerPublishVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ControllerUnpublishVolumeResponse>>
Client::controllerUnpublishVolume(ControllerUnpublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerUnpublishVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ValidateVolumeCapabilitiesResponse>>
Client::validateVolumeCapabilities(ValidateVolumeCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ValidateVolumeCapabilities),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ListVolumesResponse>>
Client::listVolumes(ListVolumesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ListVolumes),
      std::move(request),
      CallOptions());
}


Future<RPCResult<GetCapacityResponse>>
Client::getCapacity(GetCapacityRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, GetCapacity),
      std::move(request),
      CallOptions());
}


Future<RPCResult<ControllerGetCapabilitiesResponse>>
Client::controllerGetCapabilities(ControllerGetCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerGetCapabilities),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodeStageVolumeResponse>>
Client::nodeStageVolume(NodeStageVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeStageVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodeUnstageVolumeResponse>>
Client::nodeUnstageVolume(NodeUnstageVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeUnstageVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodePublishVolumeResponse>>
Client::nodePublishVolume(NodePublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodePublishVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodeUnpublishVolumeResponse>>
Client::nodeUnpublishVolume(NodeUnpublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeUnpublishVolume),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodeGetIdResponse>>
Client::nodeGetId(NodeGetIdRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeGetId),
      std::move(request),
      CallOptions());
}


Future<RPCResult<NodeGetCapabilitiesResponse>>
Client::nodeGetCapabilities(NodeGetCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeGetCapabilities),
      std::move(request),
      CallOptions());
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
