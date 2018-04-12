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

#include <process/gtest.hpp>

#include <stout/lambda.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/tests/utils.hpp>

#include "csi/client.hpp"

#include "tests/mock_csi_plugin.hpp"

using std::string;

using mesos::csi::v0::Client;

using process::Future;

using process::grpc::Channel;

using process::grpc::client::Runtime;

using testing::TestParamInfo;
using testing::Values;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

struct RPCParam
{
  struct Printer
  {
    const string& operator()(const TestParamInfo<RPCParam>& info) const
    {
      return info.param.name;
    }
  };

  template <typename Request, typename Response>
  RPCParam(const string& _name, Future<Response>(Client::*rpc)(const Request&))
    : name(_name),
      call([=](const Channel& channel, const Runtime runtime) {
        return (Client(channel, runtime).*rpc)(Request())
          .then([] { return Nothing(); });
      }) {}

  string name;
  lambda::function<Future<Nothing>(const Channel&, const Runtime&)> call;
};


class CSIClientTest
  : public TemporaryDirectoryTest,
    public WithParamInterface<RPCParam>
{
protected:
  virtual void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    Try<Channel> _channel = plugin.startup();
    ASSERT_SOME(_channel);

    channel = _channel.get();
  }

  virtual void TearDown() override
  {
    runtime.terminate();
    AWAIT_ASSERT_READY(runtime.wait());

    ASSERT_SOME(plugin.shutdown());
  }

  MockCSIPlugin plugin;
  Option<process::grpc::Channel> channel;
  process::grpc::client::Runtime runtime;
};


#define RPC_PARAM(method) \
  RPCParam(strings::replace(#method, "::", "_"), &method)


INSTANTIATE_TEST_CASE_P(
    Identity,
    CSIClientTest,
    Values(
        RPC_PARAM(Client::GetPluginInfo),
        RPC_PARAM(Client::GetPluginCapabilities),
        RPC_PARAM(Client::Probe)),
    RPCParam::Printer());

INSTANTIATE_TEST_CASE_P(
    Controller,
    CSIClientTest,
    Values(
        RPC_PARAM(Client::CreateVolume),
        RPC_PARAM(Client::DeleteVolume),
        RPC_PARAM(Client::ControllerPublishVolume),
        RPC_PARAM(Client::ControllerUnpublishVolume),
        RPC_PARAM(Client::ValidateVolumeCapabilities),
        RPC_PARAM(Client::ListVolumes),
        RPC_PARAM(Client::GetCapacity),
        RPC_PARAM(Client::ControllerGetCapabilities)),
    RPCParam::Printer());

INSTANTIATE_TEST_CASE_P(
    Node,
    CSIClientTest,
    Values(
        RPC_PARAM(Client::NodeStageVolume),
        RPC_PARAM(Client::NodeUnstageVolume),
        RPC_PARAM(Client::NodePublishVolume),
        RPC_PARAM(Client::NodeUnpublishVolume),
        RPC_PARAM(Client::NodeGetId),
        RPC_PARAM(Client::NodeGetCapabilities)),
    RPCParam::Printer());


// This test verifies that the all methods of CSI clients work.
TEST_P(CSIClientTest, Call)
{
  Future<Nothing> call = GetParam().call(channel.get(), runtime);
  AWAIT_EXPECT_READY(call);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
