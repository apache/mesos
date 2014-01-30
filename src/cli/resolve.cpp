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

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "master/detector.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using process::Future;
using process::UPID;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " <master>" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  flags::FlagsBase flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Duration timeout;
  flags.add(&timeout,
            "timeout",
            "How long to wait to resolve master",
            Seconds(5));

  bool verbose;
  flags.add(&verbose,
            "verbose",
            "Be verbose",
            false);

  // Load flags from environment and command line.
  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (help) {
    usage(argv[0], flags);
    return -1;
  }

  if (argc < 2) {
    usage(argv[0], flags);
    return -1;
  }

  Option<string> master = None();

  // Find 'master' argument (the only argument not prefixed by '--').
  for (int i = 1; i < argc; i++) {
    const std::string arg(strings::trim(argv[i]));
    if (arg.find("--") != 0) {
      if (master.isSome()) {
        // There should only be one non-flag argument.
        cerr << "Ambiguous 'master': "
             << master.get() << " and " << arg << endl;
        usage(argv[0], flags);
        return -1;
      } else {
        master = arg;
      }
    }
  }

  if (master.isNone()) {
    cerr << "Missing 'master'" << endl;
    usage(argv[0], flags);
    return -1;
  }

  Try<MasterDetector*> detector = MasterDetector::create(master.get());

  if (detector.isError()) {
    cerr << "Failed to create a master detector: " << detector.error() << endl;
    return -1;
  }

  Future<Option<MasterInfo> > masterInfo = detector.get()->detect();

  if (!masterInfo.await(timeout)) {
    cerr << "Failed to detect master from '" << master.get()
         << "' within " << timeout << endl;
    return -1;
  } else {
    CHECK(!masterInfo.isDiscarded());

    if (masterInfo.isFailed()) {
      cerr << "Failed to detect master from '" << master.get()
           << "': " << masterInfo.failure() << endl;
      return -1;
    }
  }

  // The future is not satisfied unless the result is Some.
  CHECK_SOME(masterInfo.get());
  cout << strings::remove(masterInfo.get().get().pid(), "master@") << endl;

  return 0;
}
