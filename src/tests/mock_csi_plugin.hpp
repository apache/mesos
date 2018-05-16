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

#define CSI_METHOD_FOREACH(macro)            \
  macro(GetPluginInfo)                       \
  macro(GetPluginCapabilities)               \
  macro(Probe)                               \
  macro(CreateVolume)                        \
  macro(DeleteVolume)                        \
  macro(ControllerPublishVolume)             \
  macro(ControllerUnpublishVolume)           \
  macro(ValidateVolumeCapabilities)          \
  macro(ListVolumes)                         \
  macro(GetCapacity)                         \
  macro(ControllerGetCapabilities)           \
  macro(NodeStageVolume)                     \
  macro(NodeUnstageVolume)                   \
  macro(NodePublishVolume)                   \
  macro(NodeUnpublishVolume)                 \
  macro(NodeGetId)                           \
  macro(NodeGetCapabilities)

#define DECLARE_MOCK_CSI_METHOD(name)        \
  MOCK_METHOD3(name, grpc::Status(           \
      grpc::ServerContext* context,          \
      const csi::v0::name##Request* request, \
      csi::v0::name##Response* response));

// Definition of a mock CSI plugin to be used in tests with gmock.
class MockCSIPlugin : public csi::v0::Identity::Service,
                      public csi::v0::Controller::Service,
                      public csi::v0::Node::Service
{
public:
  MockCSIPlugin();

  CSI_METHOD_FOREACH(DECLARE_MOCK_CSI_METHOD)

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
