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

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>

#include <mesos/agent/agent.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

namespace http = process::http;

#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

using mesos::agent::Call;
using mesos::agent::ProcessIO;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::IOSwitchboardServer;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

#ifndef __WINDOWS__
class IOSwitchboardServerTest : public TemporaryDirectoryTest {};


TEST_F(IOSwitchboardServerTest, RedirectLog)
{
  int stdoutPipe[2];
  int stderrPipe[2];

  Try<int> nullFd = os::open("/dev/null", O_RDWR);
  ASSERT_SOME(nullFd);

  Try<Nothing> pipe = os::pipe(stdoutPipe);
  ASSERT_SOME(pipe);

  pipe = os::pipe(stderrPipe);
  ASSERT_SOME(pipe);

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

  string socketPath = path::join(
      sandbox.get(),
      "mesos-io-switchboard-" + UUID::random().toString());

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      false,
      nullFd.get(),
      stdoutPipe[0],
      stdoutFd.get(),
      stderrPipe[0],
      stderrFd.get(),
      socketPath);

  ASSERT_SOME(server);

  Future<Nothing> runServer  = server.get()->run();

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
  Try<int> nullFd = os::open("/dev/null", O_RDWR);
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

  string socketPath = path::join(
      sandbox.get(),
      "mesos-io-switchboard-" + UUID::random().toString());

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

  Call call;
  call.set_type(Call::ATTACH_CONTAINER_OUTPUT);

  Call::AttachContainerOutput* attach = call.mutable_attach_container_output();
  attach->mutable_container_id()->set_value(UUID::random().toString());

  http::Request request;
  request.method = "POST";
  request.url.domain = "";
  request.url.path = "/";
  request.keepAlive = true;
  request.headers["Accept"] = APPLICATION_STREAMING_JSON;
  request.headers["Content-Type"] = APPLICATION_JSON;
  request.body = stringify(JSON::protobuf(call));

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection = http::connect(address.get());
  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = connection.send(request, true);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response.get().type);

  Option<http::Pipe::Reader> reader = response.get().reader;
  ASSERT_SOME(reader);

  auto deserializer = [](const string& body) {
    Try<JSON::Value> value = JSON::parse(body);
    Try<agent::ProcessIO> parse =
      ::protobuf::parse<agent::ProcessIO>(value.get());
    return parse;
  };

  recordio::Reader<agent::ProcessIO> responseDecoder(
      ::recordio::Decoder<agent::ProcessIO>(deserializer),
      reader.get());

  string stdoutReceived = "";
  string stderrReceived = "";

  while (true) {
    Future<Result<agent::ProcessIO>> _message = responseDecoder.read();
    AWAIT_READY(_message);

    if (_message->isNone()) {
      break;
    }

    ASSERT_SOME(_message.get());

    agent::ProcessIO message = _message.get().get();

    ASSERT_EQ(agent::ProcessIO::DATA, message.type());

    ASSERT_TRUE(message.data().type() == agent::ProcessIO::Data::STDOUT ||
                message.data().type() == agent::ProcessIO::Data::STDERR);

    if (message.data().type() == agent::ProcessIO::Data::STDOUT) {
      stdoutReceived += message.data().data();
    } else if (message.data().type() == agent::ProcessIO::Data::STDERR) {
      stderrReceived += message.data().data();
    }
  }

  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  AWAIT_ASSERT_READY(runServer);

  os::close(nullFd.get());
  os::close(stdoutFd.get());
  os::close(stderrFd.get());

  EXPECT_EQ(data, stdoutReceived);
  EXPECT_EQ(data, stderrReceived);
}


TEST_F(IOSwitchboardServerTest, AttachInput)
{
  // We use a pipe in this test to prevent the switchboard from
  // reading EOF on its `stdoutFromFd` until we are ready for the
  // switchboard to terminate.
  int stdoutPipe[2];

  Try<Nothing> pipe = os::pipe(stdoutPipe);
  ASSERT_SOME(pipe);

  Try<int> nullFd = os::open("/dev/null", O_RDWR);
  ASSERT_SOME(nullFd);

  string stdinPath = path::join(sandbox.get(), "stdin");
  Try<int> stdinFd = os::open(
      stdinPath,
      O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(stdinFd);

  string socketPath = path::join(
      sandbox.get(),
      "mesos-io-switchboard-" + UUID::random().toString());

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
  request.headers["Content-Type"] = APPLICATION_STREAMING_JSON;

  Try<unix::Address> address = unix::Address::create(socketPath);
  ASSERT_SOME(address);

  Future<http::Connection> _connection = http::connect(address.get());
  AWAIT_READY(_connection);
  http::Connection connection = _connection.get();

  Future<http::Response> response = connection.send(request);

  ::recordio::Encoder<mesos::agent::Call> encoder(lambda::bind(
        serialize, ContentType::STREAMING_JSON, lambda::_1));

  Call call;
  call.set_type(Call::ATTACH_CONTAINER_INPUT);

  Call::AttachContainerInput* attach = call.mutable_attach_container_input();
  attach->set_type(Call::AttachContainerInput::CONTAINER_ID);
  attach->mutable_container_id()->set_value(UUID::random().toString());

  writer.write(encoder.encode(call));

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

    writer.write(encoder.encode(call));
  }

  writer.close();

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

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


class IOSwitchboardTest
  : public ContainerizerTest<slave::MesosContainerizer> {};


// The test verifies the output redirection of the container with TTY
// allocated for the container.
TEST_F(IOSwitchboardTest, OutputRedirectionWithTTY)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";
  flags.io_switchboard_enable_server = true;

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

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

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executorInfo,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  EXPECT_SOME_EQ("HelloWorld", os::read(path::join(directory.get(), "stdout")));
}

#endif // __WINDOWS__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
