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

#include <gtest/gtest.h>

#include <string>

#include <process/process.hpp>

#include <stout/os.hpp>

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "tests/environment.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using std::cerr;
using std::endl;
using std::string;


void usage(const char* argv0, const Configurator& configurator)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  flags::Flags<logging::Flags> flags;

  // We log to stderr by default, but when running tests we'd prefer
  // less junk to fly by, so force one to specify the verbosity.
  bool verbose;

  flags.add(&verbose,
            "verbose",
            "Log all severity levels to stderr",
            false);

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Configurator configurator(flags);
  Configuration configuration;
  try {
    configuration = configurator.load(argc, argv);
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    usage(argv[0], configurator);
    exit(1);
  }

  flags.load(configuration.getMap());

  if (help) {
    usage(argv[0], configurator);
    cerr << endl;
    testing::InitGoogleTest(&argc, argv); // Get usage from gtest too.
    exit(1);
  }

  // Initialize libprocess.
  process::initialize();

  // Be quiet by default!
  if (!verbose) {
    flags.quiet = true;
  }

  // Initialize logging.
  logging::initialize(argv[0], flags);

  // Initialize gmock/gtest.
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";

  // Get the absolute path to the source (i.e., root) directory.
  Try<string> path = os::realpath(SOURCE_DIR);
  CHECK(path.isSome()) << "Error getting source directory " << path.error();
  mesosSourceDirectory = path.get();

  std::cout << "Source directory: " << mesosSourceDirectory << std::endl;

  // Get absolute path to the build directory.
  path = os::realpath(BUILD_DIR);
  CHECK(path.isSome()) << "Error getting build directory " << path.error();
  mesosBuildDirectory = path.get();

  std::cout << "Build directory: " << mesosBuildDirectory << std::endl;

  ::testing::AddGlobalTestEnvironment(new Environment());

  return RUN_ALL_TESTS();
}
