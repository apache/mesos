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

#ifndef __CSI_CLIENT_HPP__
#define __CSI_CLIENT_HPP__

#include <csi/spec.hpp>

#include <process/grpc.hpp>

#include "csi/rpc.hpp"

namespace mesos {
namespace csi {
namespace v0 {

class Client
{
public:
  Client(const process::grpc::client::Connection& _connection,
         const process::grpc::client::Runtime& _runtime)
    : connection(_connection), runtime(_runtime) {}

  template <RPC rpc>
  process::Future<Try<Response<rpc>, process::grpc::StatusError>> call(
      Request<rpc> request);

private:
  process::grpc::client::Connection connection;
  process::grpc::client::Runtime runtime;
};


template <>
process::Future<Try<GetPluginInfoResponse, process::grpc::StatusError>>
Client::call<GET_PLUGIN_INFO>(GetPluginInfoRequest request);


template <>
process::Future<Try<GetPluginCapabilitiesResponse, process::grpc::StatusError>>
Client::call<GET_PLUGIN_CAPABILITIES>(GetPluginCapabilitiesRequest request);


template <>
process::Future<Try<ProbeResponse, process::grpc::StatusError>>
Client::call<PROBE>(ProbeRequest request);


template <>
process::Future<Try<CreateVolumeResponse, process::grpc::StatusError>>
Client::call<CREATE_VOLUME>(CreateVolumeRequest request);


template <>
process::Future<Try<DeleteVolumeResponse, process::grpc::StatusError>>
Client::call<DELETE_VOLUME>(DeleteVolumeRequest request);


template <>
process::Future<
    Try<ControllerPublishVolumeResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_PUBLISH_VOLUME>(ControllerPublishVolumeRequest request);


template <>
process::Future<
    Try<ControllerUnpublishVolumeResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_UNPUBLISH_VOLUME>(
    ControllerUnpublishVolumeRequest request);


template <>
process::Future<
    Try<ValidateVolumeCapabilitiesResponse, process::grpc::StatusError>>
Client::call<VALIDATE_VOLUME_CAPABILITIES>(
    ValidateVolumeCapabilitiesRequest request);


template <>
process::Future<Try<ListVolumesResponse, process::grpc::StatusError>>
Client::call<LIST_VOLUMES>(ListVolumesRequest request);


template <>
process::Future<Try<GetCapacityResponse, process::grpc::StatusError>>
Client::call<GET_CAPACITY>(GetCapacityRequest request);


template <>
process::Future<
    Try<ControllerGetCapabilitiesResponse, process::grpc::StatusError>>
Client::call<CONTROLLER_GET_CAPABILITIES>(
    ControllerGetCapabilitiesRequest request);


template <>
process::Future<Try<NodeStageVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_STAGE_VOLUME>(NodeStageVolumeRequest request);


template <>
process::Future<Try<NodeUnstageVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_UNSTAGE_VOLUME>(NodeUnstageVolumeRequest request);


template <>
process::Future<Try<NodePublishVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_PUBLISH_VOLUME>(NodePublishVolumeRequest request);


template <>
process::Future<Try<NodeUnpublishVolumeResponse, process::grpc::StatusError>>
Client::call<NODE_UNPUBLISH_VOLUME>(NodeUnpublishVolumeRequest request);


template <>
process::Future<Try<NodeGetIdResponse, process::grpc::StatusError>>
Client::call<NODE_GET_ID>(NodeGetIdRequest request);


template <>
process::Future<Try<NodeGetCapabilitiesResponse, process::grpc::StatusError>>
Client::call<NODE_GET_CAPABILITIES>(NodeGetCapabilitiesRequest request);

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_CLIENT_HPP__
