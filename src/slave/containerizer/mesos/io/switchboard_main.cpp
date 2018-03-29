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

#include <array>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>

#include <stout/abort.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "logging/logging.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

namespace io = process::io;

using namespace mesos::internal::slave;

using process::Future;
using process::Owned;

// We use a pipe to forcibly unblock an io switchboard server and
// cause it to gracefully shutdown after receiving a SIGTERM signal.
//
// TODO(klueska): Ideally we would use `libevent` or `libev`s built in
// support to defer a signal handler to a thread, but we currently
// don't expose this through libprocess. Once we do expose this, we
// should change this logic to use it.
std::array<int_fd, 2> unblockFds;

static void sigtermHandler(int sig)
{
  int write = -1;
  do {
    write = ::write(unblockFds[1], "\0", 1);
  } while (write == -1 && errno == EINTR);

  ::close(unblockFds[1]);

  if (write == -1) {
    ABORT("Failed to terminate io switchboard gracefully");
  }
}


int main(int argc, char** argv)
{
  IOSwitchboardServer::Flags flags;

  // Load and validate flags from the environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], false);

  // Verify non-optional flags have valid values.
  if (flags.stdin_to_fd.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--stdin_to_fd' is missing");
  }

  if (flags.stdout_from_fd.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--stdout_from_fd' is missing");
  }

  if (flags.stdout_to_fd.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--stdout_to_fd' is missing");
  }

  if (flags.stderr_from_fd.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--stderr_from_fd' is missing");
  }

  if (flags.stderr_to_fd.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--stderr_to_fd' is missing");
  }

  if (flags.socket_path.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("'--socket_path' is missing");
  }

  Try<std::array<int_fd, 2>> pipe = os::pipe();
  if (pipe.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to create pipe for signaling unblock:"
                       << " " + pipe.error();
  }

  unblockFds = pipe.get();

  if (os::signals::install(SIGTERM, sigtermHandler) != 0) {
    EXIT(EXIT_FAILURE) << "Failed to register signal"
                       << " '" + stringify(strsignal(SIGTERM)) << "'";
  }

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      flags.tty,
      flags.stdin_to_fd.get(),
      flags.stdout_from_fd.get(),
      flags.stdout_to_fd.get(),
      flags.stderr_from_fd.get(),
      flags.stderr_to_fd.get(),
      flags.socket_path.get(),
      flags.wait_for_connection,
      flags.heartbeat_interval);

  if (server.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to create the io switchboard server:"
                          " " << server.error();
  }

  io::poll(unblockFds[0], io::READ)
    .onAny([server](const Future<short>& future) {
      os::close(unblockFds[0]);

      if (!future.isReady()) {
        EXIT(EXIT_FAILURE) << "Failed while polling on 'unblockFds[0]': "
                           << (future.isFailed() ?
                               future.failure() :
                               "discarded");
      }

      server.get()->unblock();
    });

  Future<Nothing> run = server.get()->run();
  run.await();

  server->reset();

  if (!run.isReady()) {
    EXIT(EXIT_FAILURE) << "The io switchboard server failed: "
                       << (run.isFailed() ? run.failure() : "discarded");
  }

  return EXIT_SUCCESS;
}
