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

#include "logging/logging.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::tests;

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

  using mesos::internal::tests::flags; // Needed to disabmiguate.

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
  if (!flags.verbose) {
    flags.quiet = true;
  }

  // Initialize logging.
  logging::initialize(argv[0], flags);

  // Initialize gmock/gtest.
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";

  std::cout << "Source directory: " << flags.source_dir << std::endl;
  std::cout << "Build directory: " << flags.build_dir << std::endl;

  // Instantiate our environment. Note that it will be managed by
  // gtest after we add it via testing::AddGlobalTestEnvironment.
  environment = new Environment();

  testing::AddGlobalTestEnvironment(environment);

  return RUN_ALL_TESTS();
}
