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

#include <string>

#include <process/grpc.hpp>

#include "csi/spec.hpp"

namespace mesos {
namespace csi {

using namespace ::csi;

class Client
{
public:
  Client(const std::string& uri,
         const process::grpc::client::Runtime& _runtime)
    : channel(uri),
      runtime(_runtime) {}

  // RPCs for the Identity service.
  process::Future<GetSupportedVersionsResponse>
    GetSupportedVersions(const GetSupportedVersionsRequest& request);

  process::Future<GetPluginInfoResponse>
    GetPluginInfo(const GetPluginInfoRequest& request);

  // RPCs for the Controller service.
  process::Future<CreateVolumeResponse>
    CreateVolume(const CreateVolumeRequest& request);

  process::Future<DeleteVolumeResponse>
    DeleteVolume(const DeleteVolumeRequest& request);

  process::Future<ControllerPublishVolumeResponse>
    ControllerPublishVolume(const ControllerPublishVolumeRequest& request);

  process::Future<ControllerUnpublishVolumeResponse>
    ControllerUnpublishVolume(const ControllerUnpublishVolumeRequest& request);

  process::Future<ValidateVolumeCapabilitiesResponse>
    ValidateVolumeCapabilities(
        const ValidateVolumeCapabilitiesRequest& request);

  process::Future<ListVolumesResponse>
    ListVolumes(const ListVolumesRequest& request);

  process::Future<GetCapacityResponse>
    GetCapacity(const GetCapacityRequest& request);

  process::Future<ControllerGetCapabilitiesResponse>
    ControllerGetCapabilities(const ControllerGetCapabilitiesRequest& request);

  // RPCs for the Node service.
  process::Future<NodePublishVolumeResponse>
    NodePublishVolume(const NodePublishVolumeRequest& request);

  process::Future<NodeUnpublishVolumeResponse>
    NodeUnpublishVolume(const NodeUnpublishVolumeRequest& request);

  process::Future<GetNodeIDResponse>
    GetNodeID(const GetNodeIDRequest& request);

  process::Future<ProbeNodeResponse>
    ProbeNode(const ProbeNodeRequest& request);

  process::Future<NodeGetCapabilitiesResponse>
    NodeGetCapabilities(const NodeGetCapabilitiesRequest& request);

private:
  process::grpc::Channel channel;
  process::grpc::client::Runtime runtime;
};

} // namespace csi {
} // namespace mesos {

#endif // __CSI_CLIENT_HPP__
