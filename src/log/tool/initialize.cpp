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

#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/error.hpp>

#include "log/replica.hpp"
#include "log/tool/initialize.hpp"

#include "logging/logging.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace log {
namespace tool {

Initialize::Flags::Flags()
{
  add(&Flags::path,
      "path",
      "Path to the log");

  add(&Flags::timeout,
      "timeout",
      "Maximum time allowed for the command to finish\n"
      "(e.g., 500ms, 1sec, etc.)");
}


Try<Nothing> Initialize::execute(int argc, char** argv)
{
  flags.setUsageMessage(
      "Usage: " + name() + " [option]\n"
      "\n"
      "This command is used to initialize the log.\n"
      "\n");

  // Configure the tool by parsing command line arguments.
  if (argc > 0 && argv != nullptr) {
    Try<flags::Warnings> load = flags.load(None(), argc, argv);
    if (load.isError()) {
      return Error(flags.usage(load.error()));
    }

    if (flags.help) {
      return Error(flags.usage());
    }

    process::initialize();
    logging::initialize(argv[0], false, flags);

    // Log any flag warnings (after logging is initialized).
    foreach (const flags::Warning& warning, load->warnings) {
      LOG(WARNING) << warning.message;
    }
  }

  if (flags.path.isNone()) {
    return Error(flags.usage("Missing required option --path"));
  }

  // Setup the timeout if specified.
  Option<Timeout> timeout = None();
  if (flags.timeout.isSome()) {
    timeout = Timeout::in(flags.timeout.get());
  }

  Replica replica(flags.path.get());

  // Get the current status of the replica.
  Future<Metadata::Status> status = replica.status();
  if (timeout.isSome()) {
    status.await(timeout->remaining());
  } else {
    status.await();
  }

  if (status.isPending()) {
    return Error("Timed out while getting replica status");
  } else if (status.isDiscarded()) {
    return Error("Failed to get status of replica (discarded future)");
  } else if (status.isFailed()) {
    return Error(status.failure());
  }

  // We only initialize a log if it is empty.
  if (status.get() != Metadata::EMPTY) {
    return Error("The log is not empty");
  }

  // Update the status of the replica to VOTING.
  Future<bool> update = replica.update(Metadata::VOTING);
  if (timeout.isSome()) {
    update.await(timeout->remaining());
  } else {
    update.await();
  }

  if (update.isPending()) {
    return Error("Timed out while setting replica status");
  } else if (update.isDiscarded()) {
    return Error("Failed to set replica status (discarded future)");
  } else if (update.isFailed()) {
    return Error(update.failure());
  }

  return Nothing();
}

} // namespace tool {
} // namespace log {
} // namespace internal {
} // namespace mesos {
