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

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <process/clock.hpp>
#include <process/address.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include <stout/os/constants.hpp>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/master/detector.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "messages/messages.hpp"

#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

namespace http = process::http;

#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

namespace paths = mesos::internal::slave::containerizer::paths;

using mesos::agent::Call;
using mesos::agent::ProcessIO;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::IOSwitchboardServer;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;

using process::Clock;
using process::Future;
using process::Owned;

using testing::Eq;

using std::map;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

#ifndef __WINDOWS__
class IOSwitchboardServerTest : public TemporaryDirectoryTest
{
protected:
  // Helper that sends `ATTACH_CONTAINER_OUTPUT` request on the given
  // `connection` and returns the response.
  //
  // TODO(vinod): Make this function more generic (e.g., sends any `Call`).
  Future<http::Response> attachOutput(
      const ContainerID& containerId,
      http::Connection connection)
  {
    Call call;
    call.set_type(Call::ATTACH_CONTAINER_OUTPUT);

    Call::AttachContainerOutput* attach =
      call.mutable_attach_container_output();

    attach->mutable_container_id()->CopyFrom(containerId);

    http::Request request;
    request.method = "POST";
    request.url.domain = "";
    request.url.path = "/";
    request.keepAlive = true;
    request.headers["Accept"] = APPLICATION_JSON;
    request.headers["Content-Type"] = APPLICATION_JSON;
    request.body = stringify(JSON::protobuf(call));

    return connection.send(request, true);
  }

  // Helper that sends an acknowledgment for the `ATTACH_CONTAINER_INPUT`
  // request.
  Future<http::Response> acknowledgeContainerInputResponse(
      http::Connection connection) const {
    http::Request request;
    request.method = "POST";
    request.type = http::Request::BODY;
    request.url.domain = "";
    request.url.path = "/acknowledge_container_input_response";

    return connection.send(request);
  }

  // Reads `ProcessIO::Data` records from the pipe `reader` until EOF is reached
  // and returns the merged stdout and stderr.
  // NOTE: It ignores any `ProcessIO::Control` records.
  //
  // TODO(vinod): Merge this with the identically named helper in api_tests.cpp.
  Future<tuple<string, string>> getProcessIOData(http::Pipe::Reader reader)
  {
    return reader.readAll()
      .then([](const string& data) -> Future<tuple<string, string>> {
        string stdoutReceived;
        string stderrReceived;

        ::recordio::Decoder decoder;

        Try<std::deque<string>> records = decoder.decode(data);

        if (records.isError()) {
          return process::Failure(records.error());
        }

        while(!records->empty()) {
          string record = std::move(records->front());
          records->pop_front();

          Try<agent::ProcessIO> processIO =
            deserialize<agent::ProcessIO>(ContentType::JSON, record);

          if (processIO.isError()) {
            return process::Failure(processIO.error());
          }

          if (processIO->data().type() == agent::ProcessIO::Data::STDOUT) {
            stdoutReceived += processIO->data().data();
          } else if (
              processIO->data().type() == agent::ProcessIO::Data::STDERR) {
            stderrReceived += processIO->data().data();
          }
        }

        return std::make_tuple(stdoutReceived, stderrReceived);
      });
  }
};


TEST_F(IOSwitchboardServerTest, RedirectLog)
{
  Try<int> nullFd = os::open(os::DEV_NULL, O_RDWR);
  ASSERT_SOME(nullFd);

  Try<std::array<int_fd, 2>> stdoutPipe_ = os::pipe();
  ASSERT_SOME(stdoutPipe_);

  const std::array<int_fd, 2>& stdoutPipe = stdoutPipe_.get();

  Try<std::array<int_fd, 2>> stderrPipe_ = os::pipe();
  ASSERT_SOME(stderrPipe_);

  const std::array<int_fd, 2>& stderrPipe = stderrPipe_.get();

  string stdoutPath = path::join(sandbox.get(), "stdout");
  Try<int> stdoutFd = os::open(
      stdoutPath,
      O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stdoutFd);

  string stderrPath = path::join(sandbox.get(), "stderr");
  Try<int> stderrFd = os::open(
      stderrPath,
      O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stderrFd);

  string socketPath = path::join(sandbox.get(), "mesos-io-switchboard");

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      nullFd.get(),
      stdoutPipe[0],
      stdoutFd.get(),
      stderrPipe[0],
      stderrFd.get(),
      socketPath);

  ASSERT_SOME(server);

  Future<Nothing> runServer = server.get()->run();

  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data.size()) < Megabytes(1)) {
    data.append(data);
  }

  Try<Nothing> write = os::write(stdoutPipe[1], data);
  ASSERT_SOME(write);

  write = os::write(stderrPipe[1], data);
  ASSERT_SOME(write);

  os::close(stdoutPipe[1]);
  os::close(stderrPipe[1]);

  AWAIT_ASSERT_READY(runServer);

  os::close(nullFd.get());
  os::close(stdoutPipe[0]);
  os::close(stderrPipe[0]);
  os::close(stdoutFd.get());
  os::close(stderrFd.get());

  Try<string> read = os::read(stdoutPath);
  ASSERT_SOME(read);

  EXPECT_EQ(data, read.get());

  read = os::read(stderrPath);
  ASSERT_SOME(read);

  EXPECT_EQ(data, read.get());
}


TEST_F(IOSwitchboardServerTest, AttachOutput)
{
  Try<int> nullFd = os::open(os::DEV_NULL, O_RDWR);
  ASSERT_SOME(nullFd);

  string stdoutPath = path::join(sandbox.get(), "stdout");
  Try<int> stdoutFd = os::open(
      stdoutPath,
      O_WRONLY | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stdoutFd);

  string stderrPath = path::join(sandbox.get(), "stderr");
  Try<int> stderrFd = os::open(
      stderrPath,
      O_WRONLY | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stderrFd);

  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data.size()) < Megabytes(1)) {
    data.append(data);
  }

  Try<Nothing> write = os::write(stdoutFd.get(), data);
  ASSERT_SOME(write);

  write = os::write(stderrFd.get(), data);
  ASSERT_SOME(write);

  os::close(stdoutFd.get());
  os::close(stderrFd.get());

  stdoutFd = os::open(stdoutPath, O_RDONLY);
  ASSERT_SOME(stdoutFd);

  stderrFd = os::open(stderrPath, O_RDONLY);
  ASSERT_SOME(stderrFd);

  string socketPath = path::join(sandbox.get(), "mesos-io-switchboard");

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      nullFd.get(),
      stdoutFd.get(),
      nullFd.get(),
      stderrFd.get(),
      nullFd.get(),
      socketPath,
      true);

  ASSERT_SOME(server);

  Future<Nothing> runServer = server.get()->run();

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection =
    http::connect(address.get(), http::Scheme::HTTP);

  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = attachOutput(containerId, connection);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  Future<tuple<string, string>> received =
    getProcessIOData(response->reader.get());

  AWAIT_READY(received);

  string stdoutReceived;
  string stderrReceived;

  tie(stdoutReceived, stderrReceived) = received.get();

  EXPECT_EQ(data, stdoutReceived);
  EXPECT_EQ(data, stderrReceived);

  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  AWAIT_ASSERT_READY(runServer);

  os::close(nullFd.get());
  os::close(stdoutFd.get());
  os::close(stderrFd.get());
}


TEST_F(IOSwitchboardServerTest, SendHeartbeat)
{
  // We use a pipe in this test to prevent the switchboard from
  // reading EOF on its `stdoutFromFd` until we are ready for the
  // switchboard to terminate.
  Try<std::array<int_fd, 2>> stdoutPipe_ = os::pipe();
  ASSERT_SOME(stdoutPipe_);

  const std::array<int_fd, 2>& stdoutPipe = stdoutPipe_.get();

  Try<int> nullFd = os::open(os::DEV_NULL, O_RDWR);
  ASSERT_SOME(nullFd);

  Duration heartbeat = Milliseconds(10);

  string socketPath = path::join(sandbox.get(), "mesos-io-switchboard");

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      nullFd.get(),
      stdoutPipe[0],
      nullFd.get(),
      nullFd.get(),
      nullFd.get(),
      socketPath,
      false,
      heartbeat);

  ASSERT_SOME(server);

  Future<Nothing> runServer = server.get()->run();

  Call call;
  call.set_type(Call::ATTACH_CONTAINER_OUTPUT);

  Call::AttachContainerOutput* attach = call.mutable_attach_container_output();
  attach->mutable_container_id()->set_value(id::UUID::random().toString());

  http::Request request;
  request.method = "POST";
  request.url.domain = "";
  request.url.path = "/";
  request.keepAlive = true;
  request.headers["Accept"] = APPLICATION_JSON;
  request.headers["Content-Type"] = APPLICATION_JSON;
  request.body = stringify(JSON::protobuf(call));

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection =
    http::connect(address.get(), http::Scheme::HTTP);

  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = connection.send(request, true);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);

  Option<http::Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  auto deserializer = [](const string& body) {
    return deserialize<agent::ProcessIO>(ContentType::JSON, body);
  };

  recordio::Reader<agent::ProcessIO> responseDecoder(
      deserializer, reader.get());

  // Wait for 5 heartbeat messages.
  Clock::pause();

  for (int i = 0; i < 5; i++) {
    Future<Result<agent::ProcessIO>> _message = responseDecoder.read();

    // Advance the clock by the heartbeat interval.
    Clock::advance(heartbeat);

    // Expect for the message to have been received by now.
    ASSERT_SOME(_message.get());

    agent::ProcessIO message = _message->get();

    EXPECT_EQ(agent::ProcessIO::CONTROL, message.type());

    EXPECT_TRUE(message.control().type() ==
                agent::ProcessIO::Control::HEARTBEAT);
  }

  // Closing the write end of the pipe will trigger the switchboard
  // to shutdown and close any outstanding connections.
  os::close(stdoutPipe[1]);

  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  AWAIT_ASSERT_READY(runServer);

  os::close(stdoutPipe[0]);
  os::close(nullFd.get());
}


TEST_F(IOSwitchboardServerTest, AttachInput)
{
  // We use a pipe in this test to prevent the switchboard from
  // reading EOF on its `stdoutFromFd` until we are ready for the
  // switchboard to terminate.
  Try<std::array<int_fd, 2>> stdoutPipe_ = os::pipe();
  ASSERT_SOME(stdoutPipe_);

  const std::array<int_fd, 2>& stdoutPipe = stdoutPipe_.get();

  Try<int> nullFd = os::open(os::DEV_NULL, O_RDWR);
  ASSERT_SOME(nullFd);

  string stdinPath = path::join(sandbox.get(), "stdin");
  Try<int> stdinFd = os::open(
      stdinPath,
      O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stdinFd);

  string socketPath = path::join(sandbox.get(), "mesos-io-switchboard");

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      stdinFd.get(),
      stdoutPipe[0],
      nullFd.get(),
      nullFd.get(),
      nullFd.get(),
      socketPath,
      false);

  ASSERT_SOME(server);

  Future<Nothing> runServer = server.get()->run();

  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data.size()) < Megabytes(1)) {
    data.append(data);
  }

  http::Pipe requestPipe;
  http::Pipe::Reader reader = requestPipe.reader();
  http::Pipe::Writer writer = requestPipe.writer();

  http::Request request;
  request.method = "POST";
  request.type = http::Request::PIPE;
  request.reader = reader;
  request.url.domain = "";
  request.url.path = "/";
  request.keepAlive = true;
  request.headers["Accept"] = APPLICATION_JSON;
  request.headers["Content-Type"] = APPLICATION_RECORDIO;
  request.headers[MESSAGE_CONTENT_TYPE] = APPLICATION_JSON;

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection = http::connect(
      address.get(), http::Scheme::HTTP);

  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = connection.send(request);

  Call call;
  call.set_type(Call::ATTACH_CONTAINER_INPUT);

  Call::AttachContainerInput* attach = call.mutable_attach_container_input();
  attach->set_type(Call::AttachContainerInput::CONTAINER_ID);
  attach->mutable_container_id()->set_value(id::UUID::random().toString());

  writer.write(::recordio::encode(serialize(ContentType::JSON, call)));

  size_t offset = 0;
  size_t chunkSize = 4096;
  while (offset < data.length()) {
    string dataChunk = data.substr(offset, chunkSize);
    offset += chunkSize;

    Call call;
    call.set_type(Call::ATTACH_CONTAINER_INPUT);

    Call::AttachContainerInput* attach = call.mutable_attach_container_input();
    attach->set_type(Call::AttachContainerInput::PROCESS_IO);

    ProcessIO* message = attach->mutable_process_io();
    message->set_type(ProcessIO::DATA);
    message->mutable_data()->set_type(ProcessIO::Data::STDIN);
    message->mutable_data()->set_data(dataChunk);

    writer.write(::recordio::encode(serialize(ContentType::JSON, call)));
  }

  writer.close();

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

  acknowledgeContainerInputResponse(connection);

  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  // Closing the write end of `stdoutPipe`
  // will trigger the switchboard to exit.
  os::close(stdoutPipe[1]);
  AWAIT_ASSERT_READY(runServer);

  os::close(stdoutPipe[0]);
  os::close(nullFd.get());
  os::close(stdinFd.get());

  Try<string> stdinData = os::read(stdinPath);
  ASSERT_SOME(stdinData);

  EXPECT_EQ(data, stdinData.get());
}


TEST_F(IOSwitchboardServerTest, ReceiveHeartbeat)
{
  // We use a pipe in this test to prevent the switchboard from
  // reading EOF on its `stdoutFromFd` until we are ready for the
  // switchboard to terminate.
  Try<std::array<int_fd, 2>> stdoutPipe_ = os::pipe();
  ASSERT_SOME(stdoutPipe_);

  const std::array<int_fd, 2>& stdoutPipe = stdoutPipe_.get();

  Try<int> nullFd = os::open(os::DEV_NULL, O_RDWR);
  ASSERT_SOME(nullFd);

  string socketPath = path::join(sandbox.get(), "mesos-io-switchboard");

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      nullFd.get(),
      stdoutPipe[0],
      nullFd.get(),
      nullFd.get(),
      nullFd.get(),
      socketPath,
      false);

  ASSERT_SOME(server);

  Future<Nothing> runServer = server.get()->run();

  http::Pipe requestPipe;
  http::Pipe::Reader reader = requestPipe.reader();
  http::Pipe::Writer writer = requestPipe.writer();

  http::Request request;
  request.method = "POST";
  request.type = http::Request::PIPE;
  request.reader = reader;
  request.url.domain = "";
  request.url.path = "/";
  request.keepAlive = true;
  request.headers["Accept"] = APPLICATION_JSON;
  request.headers["Content-Type"] = APPLICATION_RECORDIO;
  request.headers[MESSAGE_CONTENT_TYPE] = APPLICATION_JSON;

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection =
    http::connect(address.get(), http::Scheme::HTTP);

  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = connection.send(request);

  Call call;
  call.set_type(Call::ATTACH_CONTAINER_INPUT);

  Call::AttachContainerInput* attach = call.mutable_attach_container_input();
  attach->set_type(Call::AttachContainerInput::CONTAINER_ID);
  attach->mutable_container_id()->set_value(id::UUID::random().toString());

  writer.write(::recordio::encode(serialize(ContentType::JSON, call)));

  // Send 5 heartbeat messages.
  Duration heartbeat = Milliseconds(10);

  for (int i = 0; i < 5; i ++) {
    Call::AttachContainerInput* attach = call.mutable_attach_container_input();
    attach->set_type(Call::AttachContainerInput::PROCESS_IO);

    ProcessIO* message = attach->mutable_process_io();
    message->set_type(agent::ProcessIO::CONTROL);
    message->mutable_control()->set_type(
        agent::ProcessIO::Control::HEARTBEAT);
    message->mutable_control()->mutable_heartbeat()
        ->mutable_interval()->set_nanoseconds(heartbeat.ns());

    writer.write(::recordio::encode(serialize(ContentType::JSON, call)));

    Clock::advance(heartbeat);
  }

  writer.close();

  // All we need to verify is that the server didn't blow up as a
  // result of receiving the heartbeats.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

  acknowledgeContainerInputResponse(connection);

  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  // Closing the write end of `stdoutPipe`
  // will trigger the switchboard to exit.
  os::close(stdoutPipe[1]);
  AWAIT_ASSERT_READY(runServer);

  os::close(stdoutPipe[0]);
  os::close(nullFd.get());
}


class IOSwitchboardTest
  : public ContainerizerTest<slave::MesosContainerizer> {};


TEST_F(IOSwitchboardTest, ContainerAttach)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  // Request a tty for the container to enable attaching.
  executorInfo.mutable_container()->set_type(ContainerInfo::MESOS);
  executorInfo.mutable_container()->mutable_tty_info();

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<http::Connection> connection = containerizer->attach(containerId);
  AWAIT_READY(connection);

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// The test verifies the output redirection of the container with TTY
// allocated for the container.
TEST_F(IOSwitchboardTest, OutputRedirectionWithTTY)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Print 'Hello' to stdout and 'World' to stderr. Since the
  // container requests a TTY. Both will be redirected to the same
  // terminal device and logged in 'stdout' in the sandbox.
  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "printf Hello; printf World 1>&2",
      "cpus:1");

  // Request a tty for the container.
  executorInfo.mutable_container()->set_type(ContainerInfo::MESOS);
  executorInfo.mutable_container()->mutable_tty_info();

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  EXPECT_SOME_EQ("HelloWorld", os::read(path::join(directory.get(), "stdout")));
}


// This test verifies that a container will be
// destroyed if its io switchboard exits unexpectedly.
TEST_F(IOSwitchboardTest, KillSwitchboardContainerDestroyed)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  ContainerID childContainerId;
  childContainerId.mutable_parent()->CopyFrom(containerId);
  childContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      childContainerId,
      createContainerConfig(
          createCommandInfo("sleep 1000"),
          None(),
          mesos::slave::ContainerClass::DEBUG),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Result<pid_t> pid = paths::getContainerIOSwitchboardPid(
        flags.runtime_dir, childContainerId);

  ASSERT_SOME(pid);

  ASSERT_EQ(0, os::kill(pid.get(), SIGKILL));

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(childContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  ASSERT_TRUE(wait.get()->has_reason());
  ASSERT_EQ(TaskStatus::REASON_IO_SWITCHBOARD_EXITED,
            wait.get()->reason());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());

  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that the io switchboard isolator recovers properly.
//
// TODO(alexr): Enable after MESOS-7023 is resolved.
TEST_F(IOSwitchboardTest, DISABLED_RecoverThenKillSwitchboardContainerDestroyed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task with tty to start the switchboard server.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->mutable_tty_info();

  // Drop the status update from the slave to the master so the
  // scheduler never receives the first task update.
  Future<StatusUpdateMessage> update =
    DROP_PROTOBUF(StatusUpdateMessage(), slave.get()->pid, master.get()->pid);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(update);

  // Restart the slave with a new containerizer.
  slave.get()->terminate();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  // Expect four task updates.
  // (1) TASK_STARTING when the task starts.
  // (2) TASK_RUNNING before recovery.
  // (3) TASK_RUNNING after recovery.
  // (4) TASK_FAILED after the io switchboard is killed.
  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFailed))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  // Make sure the task comes back as running.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Kill the io switchboard for the task.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  Result<pid_t> pid = paths::getContainerIOSwitchboardPid(
        flags.runtime_dir, *containers->begin());

  ASSERT_SOME(pid);

  ASSERT_EQ(0, os::kill(pid.get(), SIGKILL));

  // Make sure the task is killed and its
  // reason is an IO switchboard failure.
  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  ASSERT_TRUE(statusFailed->has_reason());
  EXPECT_EQ(TaskStatus::REASON_IO_SWITCHBOARD_EXITED, statusFailed->reason());

  driver.stop();
  driver.join();
}


// This test verifies that a container can be attached after a slave restart.
TEST_F(IOSwitchboardTest, ContainerAttachAfterSlaveRestart)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  Future<Nothing> _ackRunning =
    FUTURE_DISPATCH(_, &slave::Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _ackStarting =
    FUTURE_DISPATCH(_, &slave::Slave::_statusUpdateAcknowledgement);

  // Launch a task with tty to start the switchboard server.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->mutable_tty_info();

  driver.launchTasks(offers.get()[0].id(), {task});

  // Ultimately wait for the `TASK_RUNNING` ack to be checkpointed.
  AWAIT_READY(statusStarting);
  AWAIT_READY(_ackStarting);
  AWAIT_READY(statusRunning);
  AWAIT_READY(_ackRunning);

  // Restart the slave with a new containerizer.
  slave.get()->terminate();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &slave::Slave::_recover);

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until containerizer is recovered.
  AWAIT_READY(_recover);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId;
  containerId.set_value(containers->begin()->value());

  Future<http::Connection> connection = containerizer->attach(containerId);
  AWAIT_READY(connection);

  driver.stop();
  driver.join();
}

#endif // __WINDOWS__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
