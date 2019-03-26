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

#ifndef __CSI_V0_CLIENT_HPP__
#define __CSI_V0_CLIENT_HPP__

#include <mesos/csi/v0.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace csi {
namespace v0 {

template <typename Response>
using RPCResult = Try<Response, process::grpc::StatusError>;


class Client
{
public:
  Client(const process::grpc::client::Connection& _connection,
         const process::grpc::client::Runtime& _runtime)
    : connection(_connection), runtime(_runtime) {}

  process::Future<RPCResult<GetPluginInfoResponse>>
  getPluginInfo(GetPluginInfoRequest request);

  process::Future<RPCResult<GetPluginCapabilitiesResponse>>
  getPluginCapabilities(GetPluginCapabilitiesRequest request);

  process::Future<RPCResult<ProbeResponse>> probe(ProbeRequest request);

  process::Future<RPCResult<CreateVolumeResponse>>
  createVolume(CreateVolumeRequest request);

  process::Future<RPCResult<DeleteVolumeResponse>>
  deleteVolume(DeleteVolumeRequest request);

  process::Future<RPCResult<ControllerPublishVolumeResponse>>
  controllerPublishVolume(ControllerPublishVolumeRequest request);

  process::Future<RPCResult<ControllerUnpublishVolumeResponse>>
  controllerUnpublishVolume(ControllerUnpublishVolumeRequest request);

  process::Future<RPCResult<ValidateVolumeCapabilitiesResponse>>
  validateVolumeCapabilities(ValidateVolumeCapabilitiesRequest request);

  process::Future<RPCResult<ListVolumesResponse>>
  listVolumes(ListVolumesRequest request);

  process::Future<RPCResult<GetCapacityResponse>>
  getCapacity(GetCapacityRequest request);

  process::Future<RPCResult<ControllerGetCapabilitiesResponse>>
  controllerGetCapabilities(ControllerGetCapabilitiesRequest request);

  process::Future<RPCResult<NodeStageVolumeResponse>>
  nodeStageVolume(NodeStageVolumeRequest request);

  process::Future<RPCResult<NodeUnstageVolumeResponse>>
  nodeUnstageVolume(NodeUnstageVolumeRequest request);

  process::Future<RPCResult<NodePublishVolumeResponse>>
  nodePublishVolume(NodePublishVolumeRequest request);

  process::Future<RPCResult<NodeUnpublishVolumeResponse>>
  nodeUnpublishVolume(NodeUnpublishVolumeRequest request);

  process::Future<RPCResult<NodeGetIdResponse>>
  nodeGetId(NodeGetIdRequest request);

  process::Future<RPCResult<NodeGetCapabilitiesResponse>>
  nodeGetCapabilities(NodeGetCapabilitiesRequest request);

private:
  process::grpc::client::Connection connection;
  process::grpc::client::Runtime runtime;
};

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_V0_CLIENT_HPP__
