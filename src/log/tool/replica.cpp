/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <sstream>

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>

#include "log/log.hpp"
#include "log/tool/initialize.hpp"
#include "log/tool/replica.hpp"

#include "logging/logging.hpp"

using namespace process;

using std::endl;
using std::ostringstream;
using std::string;

namespace mesos {
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

  add(&Flags::help,
      "help",
      "Prints the help message",
      false);
}


string Replica::usage(const string& argv0) const
{
  ostringstream out;

  out << "Usage: " << argv0 << " " << name() << " [OPTIONS]" << endl
      << endl
      << "This command is used to start a replica server" << endl
      << endl
      << "Supported OPTIONS:" << endl
      << flags.usage();

  return out.str();
}


Try<Nothing> Replica::execute(int argc, char** argv)
{
  // Configure the tool by parsing command line arguments.
  if (argc > 0 && argv != NULL) {
    Try<Nothing> load = flags.load(None(), argc, argv);
    if (load.isError()) {
      return Error(load.error() + "\n\n" + usage(argv[0]));
    }

    if (flags.help) {
      return Error(usage(argv[0]));
    }

    process::initialize();
    logging::initialize(argv[0], flags);
  }

  if (flags.quorum.isNone()) {
    return Error("Missing flag '--quorum'");
  }

  if (flags.path.isNone()) {
    return Error("Missing flag '--path'");
  }

  if (flags.servers.isNone()) {
    return Error("Missing flag '--servers'");
  }

  if (flags.znode.isNone()) {
    return Error("Missing flag '--znode'");
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
} // namespace mesos {
