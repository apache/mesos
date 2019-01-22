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

#ifndef __TESTS_MOCKCSIPLUGIN_HPP__
#define __TESTS_MOCKCSIPLUGIN_HPP__

#include <memory>
#include <string>

#include <csi/spec.hpp>

#include <gmock/gmock.h>

#include <grpcpp/grpcpp.h>

#include <process/grpc.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

// Definition of a mock CSI plugin to be used in tests with gmock.
class MockCSIPlugin : public csi::v0::Identity::Service,
                      public csi::v0::Controller::Service,
                      public csi::v0::Node::Service
{
public:
  MockCSIPlugin();

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

  Try<process::grpc::client::Connection> startup(
      const Option<std::string>& address = None());
  Try<Nothing> shutdown();

private:
  std::unique_ptr<grpc::Server> server;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCKCSIPLUGIN_HPP__
