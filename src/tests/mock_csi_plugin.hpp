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

#ifndef __TESTS_MOCK_CSI_PLUGIN_HPP__
#define __TESTS_MOCK_CSI_PLUGIN_HPP__

#include <memory>
#include <string>

#include <gmock/gmock.h>

#include <grpcpp/grpcpp.h>

#include <mesos/csi/v0.hpp>
#include <mesos/csi/v1.hpp>

#include <process/grpc.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

// Definition of a mock CSI plugin to be used in tests with gmock.
class MockCSIPlugin
  : public csi::v0::Identity::Service,
    public csi::v0::Controller::Service,
    public csi::v0::Node::Service,
    public csi::v1::Identity::Service,
    public csi::v1::Controller::Service,
    public csi::v1::Node::Service
{
public:
  MockCSIPlugin();

  // CSI v0 RPCs.

  MOCK_METHOD3(GetPluginInfo, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::GetPluginInfoRequest*,
      csi::v0::GetPluginInfoResponse*));

  MOCK_METHOD3(GetPluginCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::GetPluginCapabilitiesRequest*,
      csi::v0::GetPluginCapabilitiesResponse*));

  MOCK_METHOD3(Probe, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ProbeRequest*,
      csi::v0::ProbeResponse*));

  MOCK_METHOD3(CreateVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::CreateVolumeRequest*,
      csi::v0::CreateVolumeResponse*));

  MOCK_METHOD3(DeleteVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::DeleteVolumeRequest*,
      csi::v0::DeleteVolumeResponse*));

  MOCK_METHOD3(ControllerPublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ControllerPublishVolumeRequest*,
      csi::v0::ControllerPublishVolumeResponse*));

  MOCK_METHOD3(ControllerUnpublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ControllerUnpublishVolumeRequest*,
      csi::v0::ControllerUnpublishVolumeResponse*));

  MOCK_METHOD3(ValidateVolumeCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ValidateVolumeCapabilitiesRequest*,
      csi::v0::ValidateVolumeCapabilitiesResponse*));

  MOCK_METHOD3(ListVolumes, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ListVolumesRequest*,
      csi::v0::ListVolumesResponse*));

  MOCK_METHOD3(GetCapacity, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::GetCapacityRequest*,
      csi::v0::GetCapacityResponse*));

  MOCK_METHOD3(ControllerGetCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::ControllerGetCapabilitiesRequest*,
      csi::v0::ControllerGetCapabilitiesResponse*));

  MOCK_METHOD3(NodeStageVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodeStageVolumeRequest*,
      csi::v0::NodeStageVolumeResponse*));

  MOCK_METHOD3(NodeUnstageVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodeUnstageVolumeRequest*,
      csi::v0::NodeUnstageVolumeResponse*));

  MOCK_METHOD3(NodePublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodePublishVolumeRequest*,
      csi::v0::NodePublishVolumeResponse*));

  MOCK_METHOD3(NodeUnpublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodeUnpublishVolumeRequest*,
      csi::v0::NodeUnpublishVolumeResponse*));

  MOCK_METHOD3(NodeGetId, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodeGetIdRequest*,
      csi::v0::NodeGetIdResponse*));

  MOCK_METHOD3(NodeGetCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v0::NodeGetCapabilitiesRequest*,
      csi::v0::NodeGetCapabilitiesResponse*));

  // CSI v1 RPCs.

  MOCK_METHOD3(GetPluginInfo, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::GetPluginInfoRequest*,
      csi::v1::GetPluginInfoResponse*));

  MOCK_METHOD3(GetPluginCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::GetPluginCapabilitiesRequest*,
      csi::v1::GetPluginCapabilitiesResponse*));

  MOCK_METHOD3(Probe, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ProbeRequest*,
      csi::v1::ProbeResponse*));

  MOCK_METHOD3(CreateVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::CreateVolumeRequest*,
      csi::v1::CreateVolumeResponse*));

  MOCK_METHOD3(DeleteVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::DeleteVolumeRequest*,
      csi::v1::DeleteVolumeResponse*));

  MOCK_METHOD3(ControllerPublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ControllerPublishVolumeRequest*,
      csi::v1::ControllerPublishVolumeResponse*));

  MOCK_METHOD3(ControllerUnpublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ControllerUnpublishVolumeRequest*,
      csi::v1::ControllerUnpublishVolumeResponse*));

  MOCK_METHOD3(ValidateVolumeCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ValidateVolumeCapabilitiesRequest*,
      csi::v1::ValidateVolumeCapabilitiesResponse*));

  MOCK_METHOD3(ListVolumes, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ListVolumesRequest*,
      csi::v1::ListVolumesResponse*));

  MOCK_METHOD3(GetCapacity, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::GetCapacityRequest*,
      csi::v1::GetCapacityResponse*));

  MOCK_METHOD3(ControllerGetCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ControllerGetCapabilitiesRequest*,
      csi::v1::ControllerGetCapabilitiesResponse*));

  MOCK_METHOD3(CreateSnapshot, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::CreateSnapshotRequest*,
      csi::v1::CreateSnapshotResponse*));

  MOCK_METHOD3(DeleteSnapshot, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::DeleteSnapshotRequest*,
      csi::v1::DeleteSnapshotResponse*));

  MOCK_METHOD3(ListSnapshots, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ListSnapshotsRequest*,
      csi::v1::ListSnapshotsResponse*));

  MOCK_METHOD3(ControllerExpandVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::ControllerExpandVolumeRequest*,
      csi::v1::ControllerExpandVolumeResponse*));

  MOCK_METHOD3(NodeStageVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeStageVolumeRequest*,
      csi::v1::NodeStageVolumeResponse*));

  MOCK_METHOD3(NodeUnstageVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeUnstageVolumeRequest*,
      csi::v1::NodeUnstageVolumeResponse*));

  MOCK_METHOD3(NodePublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodePublishVolumeRequest*,
      csi::v1::NodePublishVolumeResponse*));

  MOCK_METHOD3(NodeUnpublishVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeUnpublishVolumeRequest*,
      csi::v1::NodeUnpublishVolumeResponse*));

  MOCK_METHOD3(NodeGetVolumeStats, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeGetVolumeStatsRequest*,
      csi::v1::NodeGetVolumeStatsResponse*));

  MOCK_METHOD3(NodeExpandVolume, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeExpandVolumeRequest*,
      csi::v1::NodeExpandVolumeResponse*));

  MOCK_METHOD3(NodeGetCapabilities, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeGetCapabilitiesRequest*,
      csi::v1::NodeGetCapabilitiesResponse*));

  MOCK_METHOD3(NodeGetInfo, grpc::Status(
      grpc::ServerContext*,
      const csi::v1::NodeGetInfoRequest*,
      csi::v1::NodeGetInfoResponse*));

  Try<process::grpc::client::Connection> startup(
      const Option<std::string>& address = None());

  Try<Nothing> shutdown();

private:
  std::unique_ptr<grpc::Server> server;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCK_CSI_PLUGIN_HPP__
