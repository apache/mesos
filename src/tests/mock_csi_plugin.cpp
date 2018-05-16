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
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

#define DECLARE_MOCK_CSI_METHOD_IMPL(name) \
  EXPECT_CALL(*this, name(_, _, _))        \
    .WillRepeatedly(Return(Status::OK));

MockCSIPlugin::MockCSIPlugin()
{
  CSI_METHOD_FOREACH(DECLARE_MOCK_CSI_METHOD_IMPL)
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
