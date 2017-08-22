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

#include <string>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/gtest.hpp>

#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/tests/utils.hpp>

#include "grpc_tests.grpc.pb.h"
#include "grpc_tests.pb.h"

namespace client = process::grpc::client;

using std::shared_ptr;
using std::string;
using std::unique_ptr;

using ::grpc::InsecureServerCredentials;
using ::grpc::Server;
using ::grpc::ServerBuilder;
using ::grpc::ServerContext;
using ::grpc::Status;

using process::Future;
using process::Promise;

using process::grpc::Channel;

using testing::_;
using testing::DoAll;
using testing::InvokeWithoutArgs;
using testing::Return;

namespace tests {

class PingPongServer : public PingPong::Service
{
public:
  PingPongServer()
  {
    EXPECT_CALL(*this, Send(_, _, _))
      .WillRepeatedly(Return(Status::OK));
  }

  MOCK_METHOD3(Send, Status(ServerContext*, const Ping* ping, Pong* pong));

  Try<Nothing> Startup(const string& address)
  {
    ServerBuilder builder;
    builder.AddListeningPort(address, InsecureServerCredentials());
    builder.RegisterService(this);

    server = builder.BuildAndStart();
    if (!server) {
      return Error("Unable to start a gRPC server.");
    }

    return Nothing();
  }

  Try<Nothing> Shutdown()
  {
    server->Shutdown();
    server->Wait();

    return Nothing();
  }

private:
  unique_ptr<Server> server;
};


class GRPCClientTest : public TemporaryDirectoryTest
{
protected:
  string server_address() const
  {
    // TODO(chhsiao): Use in-process tranport instead of a Unix domain
    // socket once gRPC supports it for Windows support.
    // https://github.com/grpc/grpc/pull/11145
    return "unix://" + path::join(sandbox.get(), "socket");
  }
};


// This test verifies that a gRPC future is properly set once the
// given call is processed by the server.
TEST_F(GRPCClientTest, Success)
{
  PingPongServer server;
  ASSERT_SOME(server.Startup(server_address()));

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  AWAIT_EXPECT_READY(pong);

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that we can have multiple outstanding gRPC calls.
TEST_F(GRPCClientTest, ConcurrentRPCs)
{
  PingPongServer server;
  ASSERT_SOME(server.Startup(server_address()));

  shared_ptr<Promise<Nothing>> processed1(new Promise<Nothing>());
  shared_ptr<Promise<Nothing>> processed2(new Promise<Nothing>());
  shared_ptr<Promise<Nothing>> processed3(new Promise<Nothing>());
  shared_ptr<Promise<Nothing>> pinged(new Promise<Nothing>());

  EXPECT_CALL(server, Send(_, _, _))
    .WillOnce(DoAll(
      InvokeWithoutArgs([=] {
        processed1->set(Nothing());
        AWAIT_READY(pinged->future());
      }),
      Return(Status::OK)))
    .WillOnce(DoAll(
      InvokeWithoutArgs([=] {
        processed2->set(Nothing());
        AWAIT_READY(pinged->future());
      }),
      Return(Status::OK)))
    .WillOnce(DoAll(
      InvokeWithoutArgs([=] {
        processed3->set(Nothing());
        AWAIT_READY(pinged->future());
      }),
      Return(Status::OK)));

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong1 = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());
  Future<Pong> pong2 = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());
  Future<Pong> pong3 = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  AWAIT_READY(processed1->future());
  AWAIT_READY(processed2->future());
  AWAIT_READY(processed3->future());

  pinged->set(Nothing());

  AWAIT_EXPECT_READY(pong1);
  AWAIT_EXPECT_READY(pong2);
  AWAIT_EXPECT_READY(pong3);

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that a gRPC future fails when the server responds
// with a status other than OK for the given call.
TEST_F(GRPCClientTest, Failed)
{
  PingPongServer server;

  EXPECT_CALL(server, Send(_, _, _))
    .WillOnce(Return(Status::CANCELLED));

  ASSERT_SOME(server.Startup(server_address()));

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  AWAIT_EXPECT_FAILED(pong);

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that a gRPC future can be discarded before the
// server processes the given call.
TEST_F(GRPCClientTest, DiscardedBeforeServerStarted)
{
  PingPongServer server;

  EXPECT_CALL(server, Send(_, _, _))
    .Times(0);

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());
  pong.discard();

  ASSERT_SOME(server.Startup(server_address()));

  AWAIT_EXPECT_DISCARDED(pong);

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that a gRPC future can be discarded before and
// when the server is processing the given call.
TEST_F(GRPCClientTest, DiscardedWhenServerProcessing)
{
  PingPongServer server;

  shared_ptr<Promise<Nothing>> processed(new Promise<Nothing>());
  shared_ptr<Promise<Nothing>> discarded(new Promise<Nothing>());

  EXPECT_CALL(server, Send(_, _, _))
    .WillOnce(DoAll(
        InvokeWithoutArgs([=] {
          processed->set(Nothing());
          AWAIT_READY(discarded->future());
        }),
        Return(Status::OK)));

  client::Runtime runtime;
  Channel channel(server_address());

  ASSERT_SOME(server.Startup(server_address()));

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());
  AWAIT_READY(processed->future());

  pong.discard();
  discarded->set(Nothing());

  AWAIT_EXPECT_DISCARDED(pong);

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that a gRPC future fails properly when the runtime
// is shut down before the server responds.
TEST_F(GRPCClientTest, ClientShutdown)
{
  PingPongServer server;

  shared_ptr<Promise<Nothing>> processed(new Promise<Nothing>());
  shared_ptr<Promise<Nothing>> shutdown(new Promise<Nothing>());

  EXPECT_CALL(server, Send(_, _, _))
    .WillOnce(DoAll(
        InvokeWithoutArgs([=] {
          processed->set(Nothing());
          AWAIT_READY(shutdown->future());
        }),
        Return(Status::OK)));

  ASSERT_SOME(server.Startup(server_address()));

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  AWAIT_READY(processed->future());

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  shutdown->set(Nothing());
  AWAIT_EXPECT_FAILED(pong);

  ASSERT_SOME(server.Shutdown());
}


// This test verifies that a gRPC future fails when it is unable to
// connect to the server.
TEST_F(GRPCClientTest, ServerUnreachable)
{
  client::Runtime runtime;
  Channel channel("nosuchhost");

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  AWAIT_EXPECT_FAILED(pong);
}


// This test verifies that a gRPC future fails properly when the server
// hangs when processing the given call.
TEST_F(GRPCClientTest, ServerTimeout)
{
  PingPongServer server;

  shared_ptr<Promise<Nothing>> done(new Promise<Nothing>());

  EXPECT_CALL(server, Send(_, _, _))
    .WillOnce(DoAll(
        InvokeWithoutArgs([=] {
          AWAIT_READY(done->future());
        }),
        Return(Status::OK)));

  ASSERT_SOME(server.Startup(server_address()));

  client::Runtime runtime;
  Channel channel(server_address());

  Future<Pong> pong = runtime.call(channel, GRPC_RPC(PingPong, Send), Ping());

  // TODO(chhsiao): The gRPC library returns a failure after the default
  // timeout (5 seconds) is passed, no matter when the `CompletionQueue`
  // is shut down. The timeout should be lowered once we support it.
  AWAIT_EXPECT_FAILED(pong);
  done->set(Nothing());

  runtime.terminate();
  AWAIT_ASSERT_READY(runtime.wait());

  ASSERT_SOME(server.Shutdown());
}

} // namespace tests {
