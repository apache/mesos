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

#include <list>
#include <string>

#include <process/clock.hpp>

#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "configurator/configurator.hpp"

#include "tests/environment.hpp"
#include "tests/filter.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

// A simple test event listener that makes sure to resume the clock
// after each test even if the previous test had a partial result
// (i.e., an ASSERT_* failed).
class ClockTestEventListener : public ::testing::EmptyTestEventListener
{
public:
  virtual void OnTestEnd(const ::testing::TestInfo&)
  {
    if (process::Clock::paused()) {
      process::Clock::resume();
    }
  }
};


// Returns true if we should enable the provided test. For now, this
// ONLY disables test cases and tests in three circumstances:
//   (1) The test case, test, or type parameter contains the string
//       'ROOT' but the test is being run via a non-root user.
//   (2) The test case, test, or type parameter contains the string
//       'CGROUPS' but cgroups are not supported on this machine.
//   (3) The test case, test, or type parameter contains the string
//       'CgroupsIsolationModule', but is being run via a non-root
//       user, or cgroups are not supported on this machine.
// TODO(benh): Provide a generic way to enable/disable tests by
// registering "filter" functions.
static bool enable(const ::testing::TestInfo& test)
{
  // We check (1), (2), and (3) from above against the test case
  // name, the test name, and the type parameter (when present).
  list<string> names;
  names.push_back(test.test_case_name());
  names.push_back(test.name());
  if (test.type_param() != NULL) {
    names.push_back(test.type_param());
  }

  foreach (const string& name, names) {
    if (strings::contains(name, "ROOT") && os::user() != "root") {
      return false;
    }

    if (strings::contains(name, "CGROUPS") && !os::exists("/proc/cgroups")) {
      return false;
    }

    if (strings::contains(name, "CgroupsIsolationModule") &&
        (os::user() != "root" || !os::exists("/proc/cgroups"))) {
      return false;
    }
  }

  return true;
}


// We use the constructor to setup specific tests by updating the
// gtest filter. We do this so that we can selectively run tests that
// require root or specific OS support (e.g., cgroups). Note that this
// should not effect any other filters that have been put in place
// either on the command line or via an environment variable.
// N.B. This MUST be done _before_ invoking RUN_ALL_TESTS.
Environment::Environment()
{
  // First we split the current filter into positive and negative
  // components (which are separated by a '-').
  const string& filter = ::testing::GTEST_FLAG(filter);
  string positive;
  string negative;

  size_t dash = filter.find('-');
  if (dash != string::npos) {
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
  for (int i = 0; i < unitTest->total_test_case_count(); i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);
    for (int j = 0; j < testCase->total_test_count(); j++) {
      const ::testing::TestInfo* testInfo = testCase->GetTestInfo(j);

      if (!enable(*testCase->GetTestInfo(j))) {
        negative.append(testInfo->test_case_name());
        negative.append(".");
        negative.append(testInfo->name());
        negative.append(":");
      }
    }
  }

  // Now update the gtest flag.
  ::testing::GTEST_FLAG(filter) = positive + "-" + negative;
}


void Environment::SetUp()
{
  // Clear any MESOS_ environment variables so they don't affect our tests.
  Configurator::clearMesosEnvironmentVars();

  // Add our test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(new FilterTestEventListener());
  listeners.Append(new ClockTestEventListener());
}


void Environment::TearDown() {}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

