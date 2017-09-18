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

#include <iostream>

#include <mesos/log/log.hpp>

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>

#include "log/tool/initialize.hpp"
#include "log/tool/replica.hpp"

#include "logging/logging.hpp"

using namespace process;

using mesos::log::Log;

namespace mesos {
namespace internal {
namespace log {
namespace tool {

Replica::Flags::Flags()
{
  add(&Flags::quorum,
      "quorum",
      "Quorum size");

  add(&Flags::path,
      "path",
      "Path to the log");

  add(&Flags::servers,
      "servers",
      "ZooKeeper servers");

  add(&Flags::znode,
      "znode",
      "ZooKeeper znode");

  add(&Flags::initialize,
      "initialize",
      "Whether to initialize the log",
      true);
}


Try<Nothing> Replica::execute(int argc, char** argv)
{
  flags.setUsageMessage(
      "Usage: " + name() + " [options]\n"
      "\n"
      "This command is used to start a replica server.\n"
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

  if (flags.quorum.isNone()) {
    return Error(flags.usage("Missing required option --quorum"));
  }

  if (flags.path.isNone()) {
    return Error(flags.usage("Missing required option --path"));
  }

  if (flags.servers.isNone()) {
    return Error(flags.usage("Missing required option --servers"));
  }

  if (flags.znode.isNone()) {
    return Error(flags.usage("Missing required option --znode"));
  }

  // Initialize the log.
  if (flags.initialize) {
    Initialize initialize;
    initialize.flags.path = flags.path;

    Try<Nothing> execution = initialize.execute();
    if (execution.isError()) {
      return Error(execution.error());
    }
  }

  // Create the log.
  Log log(
      flags.quorum.get(),
      flags.path.get(),
      flags.servers.get(),
      Seconds(10),
      flags.znode.get());

  // Loop forever.
  Future<Nothing>().get();

  return Nothing();
}

} // namespace tool {
} // namespace log {
} // namespace internal {
} // namespace mesos {
