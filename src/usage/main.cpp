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

#include <unistd.h> // For pid_t and STDOUT_FILENO.

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


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Flags flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (help) {
    usage(argv[0], flags);
    return 0;
  }

  if (flags.pid.isNone()) {
    cerr << "Missing pid" << endl;
    usage(argv[0], flags);
    return -1;
  }

  Try<ResourceStatistics> statistics = mesos::internal::usage(flags.pid.get());

  if (statistics.isError()) {
    cerr << "Failed to get usage: " << statistics.error() << endl;
    return -1;
  }

  if (flags.recordio) {
    Try<Nothing> write = protobuf::write(STDOUT_FILENO, statistics.get());
    if (write.isError()) {
      cerr << "Failed to write record: " << write.error() << endl;
      return -1;
    }
  } else {
    cout << stringify(JSON::Protobuf(statistics.get())) << endl;
  }

  return 0;
}
