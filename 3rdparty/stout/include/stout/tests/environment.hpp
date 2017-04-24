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

#ifndef __STOUT_TESTS_ENVIRONMENT_HPP__
#define __STOUT_TESTS_ENVIRONMENT_HPP__

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/strings.hpp>

namespace stout {
namespace internal {
namespace tests {

class TestFilter
{
public:
  TestFilter() = default;
  virtual ~TestFilter() = default;

  // Returns true iff the test should be disabled.
  virtual bool disable(const ::testing::TestInfo* test) const = 0;

  // Returns whether the test name or parameterization matches the pattern.
  static bool matches(
      const ::testing::TestInfo* test,
      const std::string& pattern)
  {
    if (strings::contains(test->test_case_name(), pattern) ||
        strings::contains(test->name(), pattern)) {
      return true;
    }

    if (test->type_param() != nullptr &&
        strings::contains(test->type_param(), pattern)) {
      return true;
    }

    return false;
  }
};


// Return list of disabled tests based on test name based filters.
static std::vector<std::string> disabled(
    const ::testing::UnitTest* unitTest,
    const std::vector<std::shared_ptr<TestFilter>>& filters)
{
  std::vector<std::string> disabled;

  for (int i = 0; i < unitTest->total_test_case_count(); i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);

    for (int j = 0; j < testCase->total_test_count(); j++) {
      const ::testing::TestInfo* test = testCase->GetTestInfo(j);

      foreach (const std::shared_ptr<TestFilter>& filter, filters) {
        if (filter->disable(test)) {
          disabled.push_back(
              test->test_case_name() + std::string(".") + test->name());
          break;
        }
      }
    }
  }

  return disabled;
}


// Used to set up and manage the test environment.
class Environment : public ::testing::Environment
{
public:
  // We use the constructor to setup specific tests by updating the
  // gtest filter. We do this so that we can selectively run tests that
  // require root or specific OS support (e.g., cgroups). Note that this
  // should not effect any other filters that have been put in place
  // either on the command line or via an environment variable.
  // NOTE: This should be done before invoking `RUN_ALL_TESTS`.
  Environment(const std::vector<std::shared_ptr<TestFilter>>& filters)
  {
    // First we split the current filter into enabled and disabled tests
    // (which are separated by a '-').
    const std::string& filtered_tests = ::testing::GTEST_FLAG(filter);

    // An empty filter indicates no tests should be run.
    if (filtered_tests.empty()) {
      return;
    }

    std::string enabled_tests;
    std::string disabled_tests;

    size_t dash = filtered_tests.find('-');
    if (dash != std::string::npos) {
      enabled_tests = filtered_tests.substr(0, dash);
      disabled_tests = filtered_tests.substr(dash + 1);
    } else {
      enabled_tests = filtered_tests;
    }

    // Use universal filter if not specified.
    if (enabled_tests.empty()) {
      enabled_tests = "*";
    }

    // Ensure disabled tests end with ":" separator before we add more.
    if (!disabled_tests.empty() && !strings::endsWith(disabled_tests, ":")) {
      disabled_tests += ":";
    }

    // Construct the filter string to handle system or platform specific tests.
    ::testing::UnitTest* unitTest = ::testing::UnitTest::GetInstance();

    disabled_tests += strings::join(":", disabled(unitTest, filters));

    // Now update the gtest flag.
    ::testing::GTEST_FLAG(filter) = enabled_tests + "-" + disabled_tests;
  }
};

} // namespace tests {
} // namespace internal {
} // namespace stout {

#endif // __STOUT_TESTS_ENVIRONMENT_HPP__
