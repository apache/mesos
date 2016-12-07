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

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>

#include <stout/abort.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

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
int unblockFds[2];


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
  IOSwitchboardServerFlags flags;

  // Load and validate flags from the environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  }

  // Verify non-optional flags have valid values.
  if (flags.stdin_to_fd == -1 ||
      flags.stdout_from_fd == -1 ||
      flags.stdout_to_fd == -1 ||
      flags.stderr_from_fd == -1 ||
      flags.stderr_to_fd == -1 ||
      flags.socket_path == "") {
    EXIT(EXIT_FAILURE) << "Illegal value in flags: " << stringify(flags);
  }

  Try<Nothing> pipe = os::pipe(unblockFds);
  if (pipe.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to create pipe for signaling unblock:"
                       << " " + pipe.error();
  }

  if (os::signals::install(SIGTERM, sigtermHandler) != 0) {
    EXIT(EXIT_FAILURE) << "Failed to register signal"
                       << " '" + stringify(strsignal(SIGTERM)) << "'";
  }

  Try<Owned<IOSwitchboardServer>> server = IOSwitchboardServer::create(
      flags.tty,
      flags.stdin_to_fd,
      flags.stdout_from_fd,
      flags.stdout_to_fd,
      flags.stderr_from_fd,
      flags.stderr_to_fd,
      flags.socket_path,
      flags.wait_for_connection);

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

  if (!run.isReady()) {
    EXIT(EXIT_FAILURE) << "The io switchboard server failed: "
                       << (run.isFailed() ? run.failure() : "discarded");
  }

  return EXIT_SUCCESS;
}
