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

#include "tests/mock_csi_plugin.hpp"

#include <algorithm>

#include <stout/bytes.hpp>

using std::string;
using std::unique_ptr;

using grpc::ChannelArguments;
using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using process::grpc::client::Connection;

using testing::_;
using testing::A;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

MockCSIPlugin::MockCSIPlugin()
{
  EXPECT_CALL(*this, GetPluginInfo(_, _, A<csi::v0::GetPluginInfoResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Enable all plugin service capabilities by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, GetPluginCapabilities(
      _, _, A<csi::v0::GetPluginCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::GetPluginCapabilitiesRequest* request,
        csi::v0::GetPluginCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_service()->set_type(
          csi::v0::PluginCapability::Service::CONTROLLER_SERVICE);

      return Status::OK;
    }));

  EXPECT_CALL(*this, Probe(_, _, A<csi::v0::ProbeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Return a success by default for testing with the test CSI plugin in
  // forwarding mode.
  EXPECT_CALL(*this, CreateVolume(_, _, A<csi::v0::CreateVolumeResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::CreateVolumeRequest* request,
        csi::v0::CreateVolumeResponse* response) {
      response->mutable_volume()->set_capacity_bytes(std::max(
          request->capacity_range().required_bytes(),
          request->capacity_range().limit_bytes()));
      response->mutable_volume()->set_id(request->name());

      return Status::OK;
    }));

  EXPECT_CALL(*this, DeleteVolume(_, _, A<csi::v0::DeleteVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerPublishVolume(
      _, _, A<csi::v0::ControllerPublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerUnpublishVolume(
      _, _, A<csi::v0::ControllerUnpublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ValidateVolumeCapabilities(
      _, _, A<csi::v0::ValidateVolumeCapabilitiesResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ListVolumes(_, _, A<csi::v0::ListVolumesResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Return an arbitrary available capacity by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, GetCapacity(_, _, A<csi::v0::GetCapacityResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::GetCapacityRequest* request,
        csi::v0::GetCapacityResponse* response) {
      response->set_available_capacity(Gigabytes(4).bytes());

      return Status::OK;
    }));

  // Enable all controller RPC capabilities by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, ControllerGetCapabilities(
      _, _, A<csi::v0::ControllerGetCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::ControllerGetCapabilitiesRequest* request,
        csi::v0::ControllerGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::LIST_VOLUMES);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::GET_CAPACITY);

      return Status::OK;
    }));

  EXPECT_CALL(*this, NodeStageVolume(
      _, _, A<csi::v0::NodeStageVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnstageVolume(
      _, _, A<csi::v0::NodeUnstageVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodePublishVolume(
      _, _, A<csi::v0::NodePublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnpublishVolume(
      _, _, A<csi::v0::NodeUnpublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeGetId(_, _, A<csi::v0::NodeGetIdResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Enable all node RPC capabilities by default for testing with the test CSI
  // plugin in forwarding mode.
  EXPECT_CALL(*this, NodeGetCapabilities(
      _, _, A<csi::v0::NodeGetCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::NodeGetCapabilitiesRequest* request,
        csi::v0::NodeGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME);

      return Status::OK;
    }));

  EXPECT_CALL(*this, GetPluginInfo(_, _, A<csi::v1::GetPluginInfoResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Enable all plugin service capabilities by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, GetPluginCapabilities(
      _, _, A<csi::v1::GetPluginCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v1::GetPluginCapabilitiesRequest* request,
        csi::v1::GetPluginCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_service()->set_type(
          csi::v1::PluginCapability::Service::CONTROLLER_SERVICE);
      response->add_capabilities()->mutable_service()->set_type(
          csi::v1::PluginCapability::Service::VOLUME_ACCESSIBILITY_CONSTRAINTS);

      return Status::OK;
    }));

  EXPECT_CALL(*this, Probe(_, _, A<csi::v1::ProbeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Return a success by default for testing with the test CSI plugin in
  // forwarding mode.
  EXPECT_CALL(*this, CreateVolume(_, _, A<csi::v1::CreateVolumeResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v1::CreateVolumeRequest* request,
        csi::v1::CreateVolumeResponse* response) {
      response->mutable_volume()->set_capacity_bytes(std::max(
          request->capacity_range().required_bytes(),
          request->capacity_range().limit_bytes()));
      response->mutable_volume()->set_volume_id(request->name());

      return Status::OK;
    }));

  EXPECT_CALL(*this, DeleteVolume(_, _, A<csi::v1::DeleteVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerPublishVolume(
      _, _, A<csi::v1::ControllerPublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerUnpublishVolume(
      _, _, A<csi::v1::ControllerUnpublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ValidateVolumeCapabilities(
      _, _, A<csi::v1::ValidateVolumeCapabilitiesResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ListVolumes(_, _, A<csi::v1::ListVolumesResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Return an arbitrary available capacity by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, GetCapacity(_, _, A<csi::v1::GetCapacityResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v1::GetCapacityRequest* request,
        csi::v1::GetCapacityResponse* response) {
      response->set_available_capacity(Gigabytes(4).bytes());

      return Status::OK;
    }));

  // Enable all controller RPC capabilities by default for testing with the test
  // CSI plugin in forwarding mode.
  EXPECT_CALL(*this, ControllerGetCapabilities(
      _, _, A<csi::v1::ControllerGetCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v1::ControllerGetCapabilitiesRequest* request,
        csi::v1::ControllerGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::LIST_VOLUMES);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::GET_CAPACITY);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::CREATE_DELETE_SNAPSHOT);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::LIST_SNAPSHOTS);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::CLONE_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::PUBLISH_READONLY);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::ControllerServiceCapability::RPC::EXPAND_VOLUME);

      return Status::OK;
    }));

  EXPECT_CALL(*this, CreateSnapshot(
      _, _, A<csi::v1::CreateSnapshotResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, DeleteSnapshot(
      _, _, A<csi::v1::DeleteSnapshotResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ListSnapshots(_, _, A<csi::v1::ListSnapshotsResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerExpandVolume(
      _, _, A<csi::v1::ControllerExpandVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeStageVolume(
      _, _, A<csi::v1::NodeStageVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnstageVolume(
      _, _, A<csi::v1::NodeUnstageVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodePublishVolume(
      _, _, A<csi::v1::NodePublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnpublishVolume(
      _, _, A<csi::v1::NodeUnpublishVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeGetVolumeStats(
      _, _, A<csi::v1::NodeGetVolumeStatsResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeExpandVolume(
      _, _, A<csi::v1::NodeExpandVolumeResponse*>()))
    .WillRepeatedly(Return(Status::OK));

  // Enable all node RPC capabilities by default for testing with the test CSI
  // plugin in forwarding mode.
  EXPECT_CALL(*this, NodeGetCapabilities(
      _, _, A<csi::v1::NodeGetCapabilitiesResponse*>()))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v1::NodeGetCapabilitiesRequest* request,
        csi::v1::NodeGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::NodeServiceCapability::RPC::GET_VOLUME_STATS);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v1::NodeServiceCapability::RPC::EXPAND_VOLUME);

      return Status::OK;
    }));

  EXPECT_CALL(*this, NodeGetInfo(_, _, A<csi::v1::NodeGetInfoResponse*>()))
    .WillRepeatedly(Return(Status::OK));
}


Try<Connection> MockCSIPlugin::startup(const Option<string>& address)
{
  ServerBuilder builder;

  if (address.isSome()) {
    builder.AddListeningPort(address.get(), InsecureServerCredentials());
  }

  builder.RegisterService(static_cast<csi::v0::Identity::Service*>(this));
  builder.RegisterService(static_cast<csi::v0::Controller::Service*>(this));
  builder.RegisterService(static_cast<csi::v0::Node::Service*>(this));
  builder.RegisterService(static_cast<csi::v1::Identity::Service*>(this));
  builder.RegisterService(static_cast<csi::v1::Controller::Service*>(this));
  builder.RegisterService(static_cast<csi::v1::Node::Service*>(this));

  server = builder.BuildAndStart();
  if (!server) {
    return Error("Unable to start a mock CSI plugin.");
  }

  return address.isSome()
    ? Connection(address.get())
    : Connection(server->InProcessChannel(ChannelArguments()));
}


Try<Nothing> MockCSIPlugin::shutdown()
{
  server->Shutdown();
  server->Wait();

  return Nothing();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
