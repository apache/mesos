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

using process::grpc::StatusError;

namespace mesos {
namespace csi {
namespace v0 {

Future<GetPluginInfoResponse> Client::GetPluginInfo(
    const GetPluginInfoRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Identity, GetPluginInfo), request)
    .then([](const Try<GetPluginInfoResponse, StatusError>& result)
        -> Future<GetPluginInfoResponse> {
      return result;
    });
}


Future<GetPluginCapabilitiesResponse> Client::GetPluginCapabilities(
    const GetPluginCapabilitiesRequest& request)
{
  return runtime
    .call(
        connection,
        GRPC_CLIENT_METHOD(Identity, GetPluginCapabilities),
        request)
    .then([](const Try<GetPluginCapabilitiesResponse, StatusError>& result)
        -> Future<GetPluginCapabilitiesResponse> {
      return result;
    });
}


Future<ProbeResponse> Client::Probe(
    const ProbeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Identity, Probe), request)
    .then([](const Try<ProbeResponse, StatusError>& result)
        -> Future<ProbeResponse> {
      return result;
    });
}


Future<CreateVolumeResponse> Client::CreateVolume(
    const CreateVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, CreateVolume), request)
    .then([](const Try<CreateVolumeResponse, StatusError>& result)
        -> Future<CreateVolumeResponse> {
      return result;
    });
}


Future<DeleteVolumeResponse> Client::DeleteVolume(
    const DeleteVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, DeleteVolume), request)
    .then([](const Try<DeleteVolumeResponse, StatusError>& result)
        -> Future<DeleteVolumeResponse> {
      return result;
    });
}


Future<ControllerPublishVolumeResponse> Client::ControllerPublishVolume(
    const ControllerPublishVolumeRequest& request)
{
  return runtime
    .call(
        connection,
        GRPC_CLIENT_METHOD(Controller, ControllerPublishVolume),
        request)
    .then([](const Try<ControllerPublishVolumeResponse, StatusError>& result)
        -> Future<ControllerPublishVolumeResponse> {
      return result;
    });
}


Future<ControllerUnpublishVolumeResponse> Client::ControllerUnpublishVolume(
    const ControllerUnpublishVolumeRequest& request)
{
  return runtime
    .call(
        connection,
        GRPC_CLIENT_METHOD(Controller, ControllerUnpublishVolume),
        request)
    .then([](const Try<ControllerUnpublishVolumeResponse, StatusError>& result)
        -> Future<ControllerUnpublishVolumeResponse> {
      return result;
    });
}


Future<ValidateVolumeCapabilitiesResponse> Client::ValidateVolumeCapabilities(
    const ValidateVolumeCapabilitiesRequest& request)
{
  return runtime
    .call(
        connection,
        GRPC_CLIENT_METHOD(Controller, ValidateVolumeCapabilities),
        request)
    .then([](const Try<ValidateVolumeCapabilitiesResponse, StatusError>& result)
        -> Future<ValidateVolumeCapabilitiesResponse> {
      return result;
    });
}


Future<ListVolumesResponse> Client::ListVolumes(
    const ListVolumesRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, ListVolumes), request)
    .then([](const Try<ListVolumesResponse, StatusError>& result)
        -> Future<ListVolumesResponse> {
      return result;
    });
}


Future<GetCapacityResponse> Client::GetCapacity(
    const GetCapacityRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, GetCapacity), request)
    .then([](const Try<GetCapacityResponse, StatusError>& result)
        -> Future<GetCapacityResponse> {
      return result;
    });
}


Future<ControllerGetCapabilitiesResponse> Client::ControllerGetCapabilities(
    const ControllerGetCapabilitiesRequest& request)
{
  return runtime
    .call(
        connection,
        GRPC_CLIENT_METHOD(Controller, ControllerGetCapabilities),
        request)
    .then([](const Try<ControllerGetCapabilitiesResponse, StatusError>& result)
        -> Future<ControllerGetCapabilitiesResponse> {
      return result;
    });
}


Future<NodeStageVolumeResponse> Client::NodeStageVolume(
    const NodeStageVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeStageVolume), request)
    .then([](const Try<NodeStageVolumeResponse, StatusError>& result)
        -> Future<NodeStageVolumeResponse> {
      return result;
    });
}


Future<NodeUnstageVolumeResponse> Client::NodeUnstageVolume(
    const NodeUnstageVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeUnstageVolume), request)
    .then([](const Try<NodeUnstageVolumeResponse, StatusError>& result)
        -> Future<NodeUnstageVolumeResponse> {
      return result;
    });
}


Future<NodePublishVolumeResponse> Client::NodePublishVolume(
    const NodePublishVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodePublishVolume), request)
    .then([](const Try<NodePublishVolumeResponse, StatusError>& result)
        -> Future<NodePublishVolumeResponse> {
      return result;
    });
}


Future<NodeUnpublishVolumeResponse> Client::NodeUnpublishVolume(
    const NodeUnpublishVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeUnpublishVolume), request)
    .then([](const Try<NodeUnpublishVolumeResponse, StatusError>& result)
        -> Future<NodeUnpublishVolumeResponse> {
      return result;
    });
}


Future<NodeGetIdResponse> Client::NodeGetId(
    const NodeGetIdRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeGetId), request)
    .then([](const Try<NodeGetIdResponse, StatusError>& result)
        -> Future<NodeGetIdResponse> {
      return result;
    });
}


Future<NodeGetCapabilitiesResponse> Client::NodeGetCapabilities(
    const NodeGetCapabilitiesRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeGetCapabilities), request)
    .then([](const Try<NodeGetCapabilitiesResponse, StatusError>& result)
        -> Future<NodeGetCapabilitiesResponse> {
      return result;
    });
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
