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

#include <signal.h>

#include <gtest/gtest.h>

#include <string>

#include <process/process.hpp>

#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/signals.hpp>

#include "logging/logging.hpp"

#include "messages/messages.hpp" // For GOOGLE_PROTOBUF_VERIFY_VERSION.

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/module.hpp"

using namespace mesos;
using namespace mesos::tests;

using std::cerr;
using std::endl;
using std::string;


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

  using mesos::tests::flags; // Needed to disabmiguate.

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  // Load flags from environment and command line but allow unknown
  // flags (since we might have gtest/gmock flags as well).
  Try<Nothing> load = flags.load("MESOS_", argc, argv, true);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    exit(1);
  }

  if (help) {
    usage(argv[0], flags);
    cerr << endl;
    testing::InitGoogleTest(&argc, argv); // Get usage from gtest too.
    exit(1);
  }

  // Initialize Modules.
  Try<Nothing> result = tests::initModules(flags.modules);
  if (result.isError()) {
    EXIT(1) << "Error initializing modules: " << result.error();
  }

  // Initialize libprocess.
  process::initialize();

  // Be quiet by default!
  if (!flags.verbose) {
    flags.quiet = true;
  }

  // Initialize logging.
  logging::initialize(argv[0], flags, true);

  // We reset the signal handler setup by 'logging::initialize()'
  // because some Mesos tests run an in-process ZooKeeper which
  // results in SIGPIPE during ZooKeeeper server shutdown. See
  // MESOS-1729 for details. This is ok because if a non-ZooKeeper
  // test throws a SIGPIPE the default handler will still terminate
  // the process, thus not masking SIGPIPE issues elsewhere.
  os::signals::reset(SIGPIPE);

  // Initialize gmock/gtest.
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";

  std::cout << "Source directory: " << flags.source_dir << std::endl;
  std::cout << "Build directory: " << flags.build_dir << std::endl;

  // Instantiate our environment. Note that it will be managed by
  // gtest after we add it via testing::AddGlobalTestEnvironment.
  environment = new tests::Environment(flags);

  testing::AddGlobalTestEnvironment(environment);

  return RUN_ALL_TESTS();
}
