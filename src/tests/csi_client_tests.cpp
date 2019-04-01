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

#include <functional>
#include <string>

#include <process/gtest.hpp>

#include <stout/nothing.hpp>

#include <stout/tests/utils.hpp>

#include "csi/v0_client.hpp"
#include "csi/v1_client.hpp"

#include "tests/mock_csi_plugin.hpp"

using std::string;

using process::Future;

using process::grpc::StatusError;

using process::grpc::client::Connection;
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
    string operator()(const TestParamInfo<RPCParam>& info) const
    {
      return info.param.name;
    }
  };

  template <typename Client, typename Request, typename Response>
  static RPCParam create(
      Future<Try<Response, StatusError>> (Client::*rpc)(Request))
  {
    return RPCParam{
      Request::descriptor()->name(),
      [rpc](const Connection& connection, const Runtime& runtime) {
        return (Client(connection, runtime).*rpc)(Request())
          .then([] { return Nothing(); });
      }};
  }

  const string name;
  const std::function<Future<Nothing>(const Connection&, const Runtime&)> call;
};


class CSIClientTest
  : public TemporaryDirectoryTest,
    public WithParamInterface<RPCParam>
{
protected:
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    Try<Connection> _connection = plugin.startup();
    ASSERT_SOME(_connection);

    connection = _connection.get();
  }

  void TearDown() override
  {
    runtime.terminate();
    AWAIT_ASSERT_READY(runtime.wait());

    ASSERT_SOME(plugin.shutdown());

    TemporaryDirectoryTest::TearDown();
  }

  MockCSIPlugin plugin;
  Option<Connection> connection;
  Runtime runtime;
};


INSTANTIATE_TEST_CASE_P(
    V0,
    CSIClientTest,
    Values(
        RPCParam::create(&csi::v0::Client::getPluginInfo),
        RPCParam::create(&csi::v0::Client::getPluginCapabilities),
        RPCParam::create(&csi::v0::Client::probe),
        RPCParam::create(&csi::v0::Client::createVolume),
        RPCParam::create(&csi::v0::Client::deleteVolume),
        RPCParam::create(&csi::v0::Client::controllerPublishVolume),
        RPCParam::create(&csi::v0::Client::controllerUnpublishVolume),
        RPCParam::create(&csi::v0::Client::validateVolumeCapabilities),
        RPCParam::create(&csi::v0::Client::listVolumes),
        RPCParam::create(&csi::v0::Client::getCapacity),
        RPCParam::create(&csi::v0::Client::controllerGetCapabilities),
        RPCParam::create(&csi::v0::Client::nodeStageVolume),
        RPCParam::create(&csi::v0::Client::nodeUnstageVolume),
        RPCParam::create(&csi::v0::Client::nodePublishVolume),
        RPCParam::create(&csi::v0::Client::nodeUnpublishVolume),
        RPCParam::create(&csi::v0::Client::nodeGetId),
        RPCParam::create(&csi::v0::Client::nodeGetCapabilities)),
    RPCParam::Printer());


INSTANTIATE_TEST_CASE_P(
    V1,
    CSIClientTest,
    Values(
        RPCParam::create(&csi::v1::Client::getPluginInfo),
        RPCParam::create(&csi::v1::Client::getPluginCapabilities),
        RPCParam::create(&csi::v1::Client::probe),
        RPCParam::create(&csi::v1::Client::createVolume),
        RPCParam::create(&csi::v1::Client::deleteVolume),
        RPCParam::create(&csi::v1::Client::controllerPublishVolume),
        RPCParam::create(&csi::v1::Client::controllerUnpublishVolume),
        RPCParam::create(&csi::v1::Client::validateVolumeCapabilities),
        RPCParam::create(&csi::v1::Client::listVolumes),
        RPCParam::create(&csi::v1::Client::getCapacity),
        RPCParam::create(&csi::v1::Client::controllerGetCapabilities),
        RPCParam::create(&csi::v1::Client::createSnapshot),
        RPCParam::create(&csi::v1::Client::deleteSnapshot),
        RPCParam::create(&csi::v1::Client::listSnapshots),
        RPCParam::create(&csi::v1::Client::controllerExpandVolume),
        RPCParam::create(&csi::v1::Client::nodeStageVolume),
        RPCParam::create(&csi::v1::Client::nodeUnstageVolume),
        RPCParam::create(&csi::v1::Client::nodePublishVolume),
        RPCParam::create(&csi::v1::Client::nodeUnpublishVolume),
        RPCParam::create(&csi::v1::Client::nodeGetVolumeStats),
        RPCParam::create(&csi::v1::Client::nodeExpandVolume),
        RPCParam::create(&csi::v1::Client::nodeGetCapabilities),
        RPCParam::create(&csi::v1::Client::nodeGetInfo)),
    RPCParam::Printer());


// This test verifies that the all methods of CSI clients work.
TEST_P(CSIClientTest, Call)
{
  AWAIT_EXPECT_READY(GetParam().call(connection.get(), runtime));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
