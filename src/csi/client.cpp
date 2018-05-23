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

template <>
Future<GetPluginInfoResponse>
Client::call<GET_PLUGIN_INFO>(
    const GetPluginInfoRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Identity, GetPluginInfo), request)
    .then([](const Try<GetPluginInfoResponse, StatusError>& result)
        -> Future<GetPluginInfoResponse> {
      return result;
    });
}


template <>
Future<GetPluginCapabilitiesResponse>
Client::call<GET_PLUGIN_CAPABILITIES>(
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


template <>
Future<ProbeResponse>
Client::call<PROBE>(
    const ProbeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Identity, Probe), request)
    .then([](const Try<ProbeResponse, StatusError>& result)
        -> Future<ProbeResponse> {
      return result;
    });
}


template <>
Future<CreateVolumeResponse>
Client::call<CREATE_VOLUME>(
    const CreateVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, CreateVolume), request)
    .then([](const Try<CreateVolumeResponse, StatusError>& result)
        -> Future<CreateVolumeResponse> {
      return result;
    });
}


template <>
Future<DeleteVolumeResponse>
Client::call<DELETE_VOLUME>(
    const DeleteVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, DeleteVolume), request)
    .then([](const Try<DeleteVolumeResponse, StatusError>& result)
        -> Future<DeleteVolumeResponse> {
      return result;
    });
}


template <>
Future<ControllerPublishVolumeResponse>
Client::call<CONTROLLER_PUBLISH_VOLUME>(
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


template <>
Future<ControllerUnpublishVolumeResponse>
Client::call<CONTROLLER_UNPUBLISH_VOLUME>(
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


template <>
Future<ValidateVolumeCapabilitiesResponse>
Client::call<VALIDATE_VOLUME_CAPABILITIES>(
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


template <>
Future<ListVolumesResponse>
Client::call<LIST_VOLUMES>(
    const ListVolumesRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, ListVolumes), request)
    .then([](const Try<ListVolumesResponse, StatusError>& result)
        -> Future<ListVolumesResponse> {
      return result;
    });
}


template <>
Future<GetCapacityResponse>
Client::call<GET_CAPACITY>(
    const GetCapacityRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Controller, GetCapacity), request)
    .then([](const Try<GetCapacityResponse, StatusError>& result)
        -> Future<GetCapacityResponse> {
      return result;
    });
}


template <>
Future<ControllerGetCapabilitiesResponse>
Client::call<CONTROLLER_GET_CAPABILITIES>(
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


template <>
Future<NodeStageVolumeResponse>
Client::call<NODE_STAGE_VOLUME>(
    const NodeStageVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeStageVolume), request)
    .then([](const Try<NodeStageVolumeResponse, StatusError>& result)
        -> Future<NodeStageVolumeResponse> {
      return result;
    });
}


template <>
Future<NodeUnstageVolumeResponse>
Client::call<NODE_UNSTAGE_VOLUME>(
    const NodeUnstageVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeUnstageVolume), request)
    .then([](const Try<NodeUnstageVolumeResponse, StatusError>& result)
        -> Future<NodeUnstageVolumeResponse> {
      return result;
    });
}


template <>
Future<NodePublishVolumeResponse>
Client::call<NODE_PUBLISH_VOLUME>(
    const NodePublishVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodePublishVolume), request)
    .then([](const Try<NodePublishVolumeResponse, StatusError>& result)
        -> Future<NodePublishVolumeResponse> {
      return result;
    });
}


template <>
Future<NodeUnpublishVolumeResponse>
Client::call<NODE_UNPUBLISH_VOLUME>(
    const NodeUnpublishVolumeRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeUnpublishVolume), request)
    .then([](const Try<NodeUnpublishVolumeResponse, StatusError>& result)
        -> Future<NodeUnpublishVolumeResponse> {
      return result;
    });
}


template <>
Future<NodeGetIdResponse>
Client::call<NODE_GET_ID>(
    const NodeGetIdRequest& request)
{
  return runtime
    .call(connection, GRPC_CLIENT_METHOD(Node, NodeGetId), request)
    .then([](const Try<NodeGetIdResponse, StatusError>& result)
        -> Future<NodeGetIdResponse> {
      return result;
    });
}


template <>
Future<NodeGetCapabilitiesResponse>
Client::call<NODE_GET_CAPABILITIES>(
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
