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

#include <mesos/master/detector.hpp>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using process::Future;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::timeout,
        "timeout",
        "How long to wait to resolve master",
        Seconds(5));

    add(&Flags::verbose, "verbose", "Be verbose", false);
  }

  Duration timeout;

  // TODO(marco): `verbose` is also a great candidate for FlagsBase.
  bool verbose;
};


int main(int argc, char** argv)
{
  Flags flags;
  flags.setUsageMessage("Usage: " + Path(argv[0]).basename() + " <master>");

  // Load flags from environment and command line, and remove
  // them from argv.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    cerr << warning.message << endl;
  }

  // 'master' argument must be the only argument left after parsing.
  if (argc != 2) {
    cerr << flags.usage("There must be only one argument: <master>") << endl;
    return EXIT_FAILURE;
  }

  string master = argv[1];
  Try<mesos::master::detector::MasterDetector*> detector =
    mesos::master::detector::MasterDetector::create(master);

  if (detector.isError()) {
    cerr << "Failed to create a master detector: " << detector.error() << endl;
    return EXIT_FAILURE;
  }

  Future<Option<MasterInfo>> masterInfo = detector.get()->detect();

  if (!masterInfo.await(flags.timeout)) {
    cerr << "Failed to detect master from '" << master
         << "' within " << flags.timeout << endl;
    return -1;
  } else {
    CHECK(!masterInfo.isDiscarded());

    if (masterInfo.isFailed()) {
      cerr << "Failed to detect master from '" << master
           << "': " << masterInfo.failure() << endl;
      return EXIT_FAILURE;
    }
  }

  // The future is not satisfied unless the result is Some.
  CHECK_SOME(masterInfo.get());
  cout << strings::remove(masterInfo->get().pid(), "master@") << endl;

  return EXIT_SUCCESS;
}
