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

using mesos::csi::Client;

using process::Future;

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
      call([=](const string& address, const Runtime runtime) {
        return (Client(address, runtime).*rpc)(Request())
          .then([] { return Nothing(); });
      }) {}

  string name;
  lambda::function<Future<Nothing>(const string&, const Runtime&)> call;
};


class CSIClientTest
  : public TemporaryDirectoryTest,
    public WithParamInterface<RPCParam>
{
protected:
  virtual void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    ASSERT_SOME(plugin.Startup(GetPluginAddress()));
  }

  virtual void TearDown() override
  {
    runtime.terminate();
    AWAIT_ASSERT_READY(runtime.wait());

    ASSERT_SOME(plugin.Shutdown());
  }

  string GetPluginAddress()
  {
    // TODO(chhsiao): Use in-process tranport instead of a Unix domain
    // socket once gRPC supports it for Windows support.
    // https://github.com/grpc/grpc/pull/11145
    return "unix://" + path::join(sandbox.get(), "socket");
  }

  MockCSIPlugin plugin;
  process::grpc::client::Runtime runtime;
};


#define RPC_PARAM(method) \
  RPCParam(strings::replace(#method, "::", "_"), &method)


INSTANTIATE_TEST_CASE_P(
    Identity,
    CSIClientTest,
    Values(
        RPC_PARAM(Client::GetSupportedVersions),
        RPC_PARAM(Client::GetPluginInfo)),
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
        RPC_PARAM(Client::NodePublishVolume),
        RPC_PARAM(Client::NodeUnpublishVolume),
        RPC_PARAM(Client::GetNodeID),
        RPC_PARAM(Client::ProbeNode),
        RPC_PARAM(Client::NodeGetCapabilities)),
    RPCParam::Printer());


// This test verifies that the all methods of CSI clients work.
TEST_P(CSIClientTest, Call)
{
  Future<Nothing> call = GetParam().call(GetPluginAddress(), runtime);
  AWAIT_EXPECT_READY(call);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
