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
#include <process/owned.hpp>

#include <stout/try.hpp>

#include "slave/containerizer/mesos/io/switchboard.hpp"

using namespace mesos::internal::slave;

using process::Future;
using process::Owned;

int main(int argc, char** argv)
{
  IOSwitchboardServerFlags flags;

  // Load and validate flags from the environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
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

  Future<Nothing> run = server.get()->run();
  run.await();

  if (!run.isReady()) {
    EXIT(EXIT_FAILURE) << "The io switchboard server failed: "
                       << (run.isFailed() ? run.failure() : "discarded");
  }

  return EXIT_SUCCESS;
}
