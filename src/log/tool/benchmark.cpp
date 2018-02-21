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

#include <stdio.h>
#include <stdlib.h>

#include <iostream>
#include <fstream>
#include <sstream>

#include <mesos/log/log.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/time.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/os/read.hpp>

#include "log/tool/initialize.hpp"
#include "log/tool/benchmark.hpp"

#include "logging/logging.hpp"

using namespace process;

using std::cout;
using std::endl;
using std::ifstream;
using std::ofstream;
using std::string;
using std::vector;

using mesos::log::Log;

namespace mesos {
namespace internal {
namespace log {
namespace tool {

Benchmark::Flags::Flags()
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

  add(&Flags::input,
      "input",
      "Path to the input trace file. Each line in the trace file\n"
      "specifies the size of the append (e.g. 100B, 2MB, etc.)");

  add(&Flags::output,
      "output",
      "Path to the output file");

  add(&Flags::type,
      "type",
      "Type of data to be written (zero, one, random)\n"
      "  zero:   all bits are 0\n"
      "  one:    all bits are 1\n"
      "  random: all bits are randomly chosen\n",
      "random");

  add(&Flags::initialize,
      "initialize",
      "Whether to initialize the log",
      true);
}


Try<Nothing> Benchmark::execute(int argc, char** argv)
{
  flags.setUsageMessage(
      "Usage: " + name() + " [options]\n"
      "\n"
      "This command is used to do performance test on the\n"
      "replicated log. It takes a trace file of write sizes\n"
      "and replay that trace to measure the latency of each\n"
      "write. The data to be written for each write can be\n"
      "specified using the --type flag.\n"
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

  if (flags.input.isNone()) {
    return Error(flags.usage("Missing required option --input"));
  }

  if (flags.output.isNone()) {
    return Error(flags.usage("Missing required option --output"));
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

  // Create the log writer.
  Log::Writer writer(&log);

  Future<Option<Log::Position>> position = writer.start();

  if (!position.await(Seconds(15))) {
    return Error("Failed to start a log writer: timed out");
  } else if (!position.isReady()) {
    return Error("Failed to start a log writer: " +
                 (position.isFailed()
                  ? position.failure()
                  : "Discarded future"));
  }

  // Statistics to output.
  vector<Bytes> sizes;
  vector<Duration> durations;
  vector<Time> timestamps;

  // Read sizes from the input trace file.
  ifstream input(flags.input->c_str());
  if (!input.is_open()) {
    return Error("Failed to open the trace file " + flags.input.get());
  }

  string line;
  while (getline(input, line)) {
    Try<Bytes> size = Bytes::parse(strings::trim(line));
    if (size.isError()) {
      return Error("Failed to parse the trace file: " + size.error());
    }

    sizes.push_back(size.get());
  }

  input.close();

  // Generate the data to be written.
  vector<string> data;
  for (size_t i = 0; i < sizes.size(); i++) {
    if (flags.type == "one") {
      data.push_back(string(sizes[i].bytes(), static_cast<char>(0xff)));
    } else if (flags.type == "random") {
      data.push_back(string(sizes[i].bytes(), os::random() % 256));
    } else {
      data.push_back(string(sizes[i].bytes(), 0));
    }
  }

  Stopwatch stopwatch;
  stopwatch.start();

  for (size_t i = 0; i < sizes.size(); i++) {
    Stopwatch stopwatch;
    stopwatch.start();

    position = writer.append(data[i]);

    if (!position.await(Seconds(10))) {
      return Error("Failed to append: timed out");
    } else if (!position.isReady()) {
      return Error("Failed to append: " +
                   (position.isFailed()
                    ? position.failure()
                    : "Discarded future"));
    } else if (position->isNone()) {
      return Error("Failed to append: exclusive write promise lost");
    }

    durations.push_back(stopwatch.elapsed());
    timestamps.push_back(Clock::now());
  }

  cout << "Total number of appends: " << sizes.size() << endl;
  cout << "Total time used: " << stopwatch.elapsed() << endl;

  // Ouput statistics.
  ofstream output(flags.output->c_str());
  if (!output.is_open()) {
    return Error("Failed to open the output file " + flags.output.get());
  }

  for (size_t i = 0; i < sizes.size(); i++) {
    output << timestamps[i]
           << " Appended " << sizes[i].bytes() << " bytes"
           << " in " << durations[i].ms() << " ms" << endl;
  }

  return Nothing();
}

} // namespace tool {
} // namespace log {
} // namespace internal {
} // namespace mesos {
