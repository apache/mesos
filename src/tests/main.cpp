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

#include "common/utils.hpp"

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using std::cerr;
using std::endl;
using std::string;


// Return true if the given test or test case can be run. This is an additional
// check for selectively running tests depending on the runtime system
// information. For example, if the name of a test contains ROOT (a special
// name), but the test is run under a non-root user, this function will return
// false indicating that this test (contains special names) should not be run.
// Other tests without special names are not affected.
static bool shouldRun(const std::string& name)
{
  if (strings::contains(name, "ROOT") &&
      utils::os::user() != "root") {
    return false;
  }

  // TODO(jieyu): Replace the cgroups check with cgroups::enabled() once the
  // cgroups module is checked in.
  if (strings::contains(name, "CGROUPS") &&
      !utils::os::exists("/proc/cgroups")) {
    return false;
  }

  return true;
}


// Setup special tests by updating the gtest filter. This function should be
// called right before RUN_ALL_TESTS().
static void setupFilter()
{
  // Split the current filter into positive filter and negative filter. The
  // current filter can be set by the environment variable. In gtest, the filter
  // is divided into two parts: the positive part and the negative part. The
  // gtest will choose to run those tests that match the positive filter but do
  // not match the negative filter. Two parts of the filter are separated by a
  // dash ('-').
  const std::string filter = ::testing::GTEST_FLAG(filter);
  std::string positive;
  std::string negative;

  size_t dash = filter.find('-');
  if (dash != std::string::npos) {
    positive = filter.substr(0, dash);
    negative = filter.substr(dash + 1);
  } else {
    positive = filter;
  }

  // Use universal filter if not specified.
  if (positive.empty()) {
    positive = "*";
  }

  // Construct the filter string to handle system or platform specific tests.
  ::testing::UnitTest* unitTest = ::testing::UnitTest::GetInstance();
  int totalTestCaseCount = unitTest->total_test_case_count();
  for (int i = 0; i < totalTestCaseCount; i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);
    int totalTestCount = testCase->total_test_count();
    for (int j = 0; j < totalTestCount; j++) {
      const ::testing::TestInfo* testInfo = testCase->GetTestInfo(j);
      std::string testCaseName = testInfo->test_case_name();
      std::string testName = testInfo->name();
      if (!shouldRun(testCaseName)) {
        negative.append(":" + testCaseName + ".*");
      } else if (!shouldRun(testName)) {
        negative.append(":" + testCaseName + "." + testName);
      }
    }
  }

  // Set the gtest flags.
  ::testing::GTEST_FLAG(filter) = positive + "-" + negative;
}


void usage(const char* argv0, const Configurator& configurator)
{
  cerr << "Usage: " << utils::os::basename(argv0) << " [...]" << endl
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
  Try<string> path = utils::os::realpath(SOURCE_DIR);
  CHECK(path.isSome()) << "Error getting source directory " << path.error();
  mesosSourceDirectory = path.get();

  std::cout << "Source directory: " << mesosSourceDirectory << std::endl;

  // Get absolute path to the build directory.
  path = utils::os::realpath(BUILD_DIR);
  CHECK(path.isSome()) << "Error getting build directory " << path.error();
  mesosBuildDirectory = path.get();

  std::cout << "Build directory: " << mesosBuildDirectory << std::endl;

  // Clear any MESOS_ environment variables so they don't affect our tests.
  Configurator::clearMesosEnvironmentVars();

  // Setup specific tests by updating the filter. We do this so that
  // we can selectively run tests that require root or specific OS
  // support (e.g., cgroups). Note that this should not effect any
  // other filters that have been put in place either on the command
  // line or via an environment variable.
  setupFilter();

  return RUN_ALL_TESTS();
}
