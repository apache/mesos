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

#include <stdint.h>

#include <iostream>
#include <sstream>

#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/error.hpp>

#include "log/replica.hpp"
#include "log/tool/read.hpp"

#include "logging/logging.hpp"

using namespace process;

using std::cout;
using std::endl;
using std::list;
using std::ostringstream;
using std::string;

namespace mesos {
namespace log {
namespace tool {

Read::Flags::Flags()
{
  add(&Flags::path,
      "path",
      "Path to the log");

  add(&Flags::from,
      "from",
      "Position from which to start reading the log");

  add(&Flags::to,
      "to",
      "Position from which to stop reading the log");

  add(&Flags::timeout,
      "timeout",
      "Maximum time allowed for the command to finish\n"
      "(e.g., 500ms, 1sec, etc.)");

  add(&Flags::help,
      "help",
      "Prints the help message",
      false);
}


string Read::usage(const string& argv0) const
{
  ostringstream out;

  out << "Usage: " << argv0 << " " << name() << " [OPTIONS]" << endl
      << endl
      << "This command is used to read the log" << endl
      << endl
      << "Supported OPTIONS:" << endl
      << flags.usage();

  return out.str();
}


Try<Nothing> Read::execute(int argc, char** argv)
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

  if (flags.path.isNone()) {
    return Error("Missing flag '--path'");
  }

  // Setup the timeout if specified.
  Option<Timeout> timeout = None();
  if (flags.timeout.isSome()) {
    timeout = Timeout::in(flags.timeout.get());
  }

  Replica replica(flags.path.get());

  // Get the beginning of the replica.
  Future<uint64_t> begin = replica.beginning();
  if (timeout.isSome()) {
    begin.await(timeout.get().remaining());
  } else {
    begin.await();
  }

  if (begin.isPending()) {
    return Error("Timed out while getting the beginning of the replica");
  } else if (begin.isDiscarded()) {
    return Error(
        "Failed to get the beginning of the replica (discarded future)");
  } else if (begin.isFailed()) {
    return Error(begin.failure());
  }

  // Get the ending of the replica.
  Future<uint64_t> end = replica.ending();
  if (timeout.isSome()) {
    end.await(timeout.get().remaining());
  } else {
    end.await();
  }

  if (end.isPending()) {
    return Error("Timed out while getting the ending of the replica");
  } else if (end.isDiscarded()) {
    return Error(
        "Failed to get the ending of the replica (discarded future)");
  } else if (end.isFailed()) {
    return Error(end.failure());
  }

  Option<uint64_t> from = flags.from;
  if (from.isNone()) {
    from = begin.get();
  }

  Option<uint64_t> to = flags.to;
  if (to.isNone()) {
    to = end.get();
  }

  LOG(INFO) << "Attempting to read the log from "
            << from.get() << " to " << to.get() << endl;

  Future<list<Action> > actions = replica.read(from.get(), to.get());
  if (timeout.isSome()) {
    actions.await(timeout.get().remaining());
  } else {
    actions.await();
  }

  if (actions.isPending()) {
    return Error("Timed out while reading the replica");
  } else if (actions.isDiscarded()) {
    return Error("Failed to read the replica (discarded future)");
  } else if (actions.isFailed()) {
    return Error(actions.failure());
  }

  foreach (const Action& action, actions.get()) {
    cout << "----------------------------------------------" << endl;
    action.PrintDebugString();
  }

  return Nothing();
}

} // namespace tool {
} // namespace log {
} // namespace mesos {
