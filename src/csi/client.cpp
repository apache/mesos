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

#include <utility>

#include "csi/client.hpp"

using process::Failure;
using process::Future;

using process::grpc::StatusError;

using process::grpc::client::CallOptions;

namespace mesos {
namespace csi {
namespace v0 {

template <>
Future<Try<GetPluginInfoResponse, process::grpc::StatusError>>
Client::call<GET_PLUGIN_INFO>(GetPluginInfoRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, GetPluginInfo),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<GetPluginCapabilitiesResponse, process::grpc::StatusError>>
Client::call<GET_PLUGIN_CAPABILITIES>(
    GetPluginCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, GetPluginCapabilities),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ProbeResponse, process::grpc::StatusError>>
Client::call<PROBE>(
    ProbeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Identity, Probe),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<CreateVolumeResponse, process::grpc::StatusError>>
Client::call<CREATE_VOLUME>(
    CreateVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, CreateVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<DeleteVolumeResponse, process::grpc::StatusError>>
Client::call<DELETE_VOLUME>(
    DeleteVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, DeleteVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ControllerPublishVolumeResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_PUBLISH_VOLUME>(
    ControllerPublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerPublishVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ControllerUnpublishVolumeResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_UNPUBLISH_VOLUME>(
    ControllerUnpublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerUnpublishVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ValidateVolumeCapabilitiesResponse, process::grpc::StatusError>>
Client::call<VALIDATE_VOLUME_CAPABILITIES>(
    ValidateVolumeCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ValidateVolumeCapabilities),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ListVolumesResponse, process::grpc::StatusError>>
Client::call<LIST_VOLUMES>(
    ListVolumesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ListVolumes),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<GetCapacityResponse, process::grpc::StatusError>>
Client::call<GET_CAPACITY>(
    GetCapacityRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, GetCapacity),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<ControllerGetCapabilitiesResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_GET_CAPABILITIES>(
    ControllerGetCapabilitiesRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Controller, ControllerGetCapabilities),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodeStageVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_STAGE_VOLUME>(
    NodeStageVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeStageVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodeUnstageVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_UNSTAGE_VOLUME>(
    NodeUnstageVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeUnstageVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodePublishVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_PUBLISH_VOLUME>(
    NodePublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodePublishVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodeUnpublishVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_UNPUBLISH_VOLUME>(
    NodeUnpublishVolumeRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeUnpublishVolume),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodeGetIdResponse, process::grpc::StatusError>>
Client::call<NODE_GET_ID>(
    NodeGetIdRequest request)
{
  return runtime.call(
      connection,
      GRPC_CLIENT_METHOD(Node, NodeGetId),
      std::move(request),
      CallOptions());
}


template <>
Future<Try<NodeGetCapabilitiesResponse, process::grpc::StatusError>>
Client::call<NODE_GET_CAPABILITIES>(
    NodeGetCapabilitiesRequest request)
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
