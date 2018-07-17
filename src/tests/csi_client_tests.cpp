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
#include <ostream>

#include <process/gtest.hpp>

#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include <stout/tests/utils.hpp>

#include "csi/client.hpp"
#include "csi/rpc.hpp"

#include "tests/mock_csi_plugin.hpp"

using std::ostream;
using std::string;

using process::Future;

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
      return strings::replace(stringify(info.param.value), ".", "_");
    }
  };

  template <csi::v0::RPC rpc>
  static RPCParam create()
  {
    return RPCParam{
      rpc,
      [](csi::v0::Client client) {
        return client
          .call<rpc>(typename csi::v0::RPCTraits<rpc>::request_type())
          .then([] { return Nothing(); });
      }
    };
  }

  const csi::v0::RPC value;
  const std::function<Future<Nothing>(csi::v0::Client)> call;
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
  }

  MockCSIPlugin plugin;
  Option<process::grpc::client::Connection> connection;
  process::grpc::client::Runtime runtime;
};


INSTANTIATE_TEST_CASE_P(
    Identity,
    CSIClientTest,
    Values(
        RPCParam::create<csi::v0::GET_PLUGIN_INFO>(),
        RPCParam::create<csi::v0::GET_PLUGIN_CAPABILITIES>(),
        RPCParam::create<csi::v0::PROBE>()),
    RPCParam::Printer());


INSTANTIATE_TEST_CASE_P(
    Controller,
    CSIClientTest,
    Values(
        RPCParam::create<csi::v0::CREATE_VOLUME>(),
        RPCParam::create<csi::v0::DELETE_VOLUME>(),
        RPCParam::create<csi::v0::CONTROLLER_PUBLISH_VOLUME>(),
        RPCParam::create<csi::v0::CONTROLLER_UNPUBLISH_VOLUME>(),
        RPCParam::create<csi::v0::VALIDATE_VOLUME_CAPABILITIES>(),
        RPCParam::create<csi::v0::LIST_VOLUMES>(),
        RPCParam::create<csi::v0::GET_CAPACITY>(),
        RPCParam::create<csi::v0::CONTROLLER_GET_CAPABILITIES>()),
    RPCParam::Printer());


INSTANTIATE_TEST_CASE_P(
    Node,
    CSIClientTest,
    Values(
        RPCParam::create<csi::v0::NODE_STAGE_VOLUME>(),
        RPCParam::create<csi::v0::NODE_UNSTAGE_VOLUME>(),
        RPCParam::create<csi::v0::NODE_PUBLISH_VOLUME>(),
        RPCParam::create<csi::v0::NODE_UNPUBLISH_VOLUME>(),
        RPCParam::create<csi::v0::NODE_GET_ID>(),
        RPCParam::create<csi::v0::NODE_GET_CAPABILITIES>()),
    RPCParam::Printer());


// This test verifies that the all methods of CSI clients work.
TEST_P(CSIClientTest, Call)
{
  AWAIT_EXPECT_READY(
      GetParam().call(csi::v0::Client(connection.get(), runtime)));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
