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

#ifndef __WINDOWS__
#include <unistd.h> // For pid_t and STDOUT_FILENO.
#endif // __WINDOWS__

#include <iostream>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "usage/usage.hpp"

using namespace mesos;

using std::cerr;
using std::cout;
using std::endl;


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::pid,
        "pid",
        "Root pid for aggregating usage/statistics");

    add(&Flags::recordio,
        "recordio",
        "Whether or not to output ResourceStatistics protobuf\n"
        "using the \"recordio\" format, i.e., the size as a \n"
        "4 byte unsigned integer followed by the serialized\n"
        "protobuf itself. By default the ResourceStatistics\n"
        "will be output as JSON",
        false);
  }

  Option<pid_t> pid;
  bool recordio;
};


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Flags flags;

  Try<flags::Warnings> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    cerr << warning.message << endl;
  }

  if (flags.pid.isNone()) {
    cerr << flags.usage("Missing required option --pid") << endl;
    return EXIT_FAILURE;
  }

  Try<ResourceStatistics> statistics = mesos::internal::usage(flags.pid.get());

  if (statistics.isError()) {
    cerr << "Failed to get usage: " << statistics.error() << endl;
    return EXIT_FAILURE;
  }

  if (flags.recordio) {
    Try<Nothing> write = protobuf::write(STDOUT_FILENO, statistics.get());
    if (write.isError()) {
      cerr << "Failed to write record: " << write.error() << endl;
      return EXIT_FAILURE;
    }
  } else {
    cout << stringify(JSON::protobuf(statistics.get())) << endl;
  }

  return EXIT_SUCCESS;
}
