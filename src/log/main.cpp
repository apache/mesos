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
#include <list>
#include <string>

#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "log/replica.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...] path/to/log"
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  flags::Flags<logging::Flags> flags;

  Option<uint64_t> from;
  flags.add(&from,
            "from",
            "Position from which to start reading in the log");

  Option<uint64_t> to;
  flags.add(&to,
            "to",
            "Position from which to stop reading in the log");

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    exit(1);
  }

  if (help) {
    usage(argv[0], flags);
    exit(1);
  }

  process::initialize();

  logging::initialize(argv[0], flags);

  string path = argv[argc - 1];

  Replica replica(path);

  process::Future<uint64_t> begin = replica.beginning();
  process::Future<uint64_t> end = replica.ending();

  begin.await();
  end.await();

  CHECK(begin.isReady());
  CHECK(end.isReady());

  if (!from.isSome()) {
    from = begin.get();
  }

  if (!to.isSome()) {
    to = end.get();
  }

  CHECK_SOME(from);
  CHECK_SOME(to);

  cerr << endl << "Attempting to read the log from "
       << from.get() << " to " << to.get() << endl << endl;

  process::Future<std::list<Action> > actions =
    replica.read(from.get(), to.get());

  actions.await();

  CHECK(!actions.isFailed()) << actions.failure();

  CHECK(actions.isReady());

  foreach (const Action& action, actions.get()) {
    cout << "----------------------------------------------" << endl;
    action.PrintDebugString();
  }

  return 0;
}
