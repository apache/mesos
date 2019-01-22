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

using std::string;
using std::unique_ptr;

using grpc::ChannelArguments;
using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using mesos::csi::v0::Controller;
using mesos::csi::v0::Identity;
using mesos::csi::v0::Node;

using process::grpc::client::Connection;

using testing::_;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

MockCSIPlugin::MockCSIPlugin()
{
  EXPECT_CALL(*this, GetPluginInfo(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  // Enable all plugin capabilities by default for testing with the test CSI
  // plugin in forwarding mode.
  EXPECT_CALL(*this, GetPluginCapabilities(_, _, _))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::GetPluginCapabilitiesRequest* request,
        csi::v0::GetPluginCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_service()->set_type(
          csi::v0::PluginCapability::Service::CONTROLLER_SERVICE);

      return Status::OK;
    }));

  EXPECT_CALL(*this, Probe(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, CreateVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, DeleteVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerPublishVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ControllerUnpublishVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ValidateVolumeCapabilities(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, ListVolumes(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, GetCapacity(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  // Enable all controller capabilities by default for testing with the test CSI
  // plugin in forwarding mode.
  EXPECT_CALL(*this, ControllerGetCapabilities(_, _, _))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::ControllerGetCapabilitiesRequest* request,
        csi::v0::ControllerGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::GET_CAPACITY);
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::ControllerServiceCapability::RPC::LIST_VOLUMES);

      return Status::OK;
    }));

  EXPECT_CALL(*this, NodeStageVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnstageVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodePublishVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeUnpublishVolume(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  EXPECT_CALL(*this, NodeGetId(_, _, _))
    .WillRepeatedly(Return(Status::OK));

  // Enable all node capabilities by default for testing with the test CSI
  // plugin in forwarding mode.
  EXPECT_CALL(*this, NodeGetCapabilities(_, _, _))
    .WillRepeatedly(Invoke([](
        ServerContext* context,
        const csi::v0::NodeGetCapabilitiesRequest* request,
        csi::v0::NodeGetCapabilitiesResponse* response) {
      response->add_capabilities()->mutable_rpc()->set_type(
          csi::v0::NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME);

      return Status::OK;
    }));
}


Try<Connection> MockCSIPlugin::startup(const Option<string>& address)
{
  ServerBuilder builder;

  if (address.isSome()) {
    builder.AddListeningPort(address.get(), InsecureServerCredentials());
  }

  builder.RegisterService(static_cast<Identity::Service*>(this));
  builder.RegisterService(static_cast<Controller::Service*>(this));
  builder.RegisterService(static_cast<Node::Service*>(this));

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
