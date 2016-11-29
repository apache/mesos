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

#include <process/owned.hpp>

#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "tests/mesos.hpp"

using mesos::internal::slave::IOSwitchboardServer;

using process::Future;
using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class IOSwitchboardTest : public TemporaryDirectoryTest {};


#ifndef __WINDOWS__
TEST_F(IOSwitchboardTest, ServerRedirectLog)
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
#endif // __WINDOWS__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
