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

#ifndef __WINDOWS__
#include <sys/wait.h>

#include <unistd.h>
#endif // __WINDOWS__

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/fs.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/temp.hpp>
#include <stout/os/touch.hpp>

using std::string;
using std::vector;

namespace stout {
namespace internal {
namespace tests {

class TestFilter
{
public:
  TestFilter() {}
  virtual ~TestFilter() {}

  // Returns true iff the test should be disabled.
  virtual bool disable(const ::testing::TestInfo* test) const = 0;

  // Returns whether the test name or parameterization matches the pattern.
  static bool matches(const ::testing::TestInfo* test, const string& pattern)
  {
    if (strings::contains(test->test_case_name(), pattern) ||
        strings::contains(test->name(), pattern)) {
      return true;
    } else if (test->type_param() != nullptr &&
               strings::contains(test->type_param(), pattern)) {
      return true;
    }

    return false;
  }
};

// Attempt to create a symlink. If not possible, disable all unit tests
// that rely on the creation of symlinks.
class SymlinkFilter : public TestFilter
{
public:
  SymlinkFilter()
  {
    const Try<string> temp_path = os::mkdtemp();
    CHECK_SOME(temp_path);

    const string file = path::join(temp_path.get(), "file");
    const string link = path::join(temp_path.get(), "link");

    CHECK_SOME(os::touch(file));

    Try<Nothing> symlinkCheck = fs::symlink(file, link);
    canCreateSymlinks = !symlinkCheck.isError();

    if (!canCreateSymlinks) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "Not able to create Symlinks, so no symlink tests will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "SYMLINK_") && !canCreateSymlinks;
  }

private:
  bool canCreateSymlinks;
};


// Return list of disabled tests based on test name based filters.
static vector<string> disabled(
    const ::testing::UnitTest* unitTest,
    const vector<std::shared_ptr<TestFilter>>& filters)
{
  vector<string> disabled;

  for (int i = 0; i < unitTest->total_test_case_count(); i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);
    for (int j = 0; j < testCase->total_test_count(); j++) {
      const ::testing::TestInfo* test = testCase->GetTestInfo(j);

      foreach (const std::shared_ptr<TestFilter>& filter, filters) {
        if (filter->disable(test)) {
          disabled.push_back(
              test->test_case_name() + string(".") + test->name());
          break;
        }
      }
    }
  }

  return disabled;
}

// Used to set up and manage the test environment.
class Environment : public ::testing::Environment {
public:
  // We use the constructor to setup specific tests by updating the
  // gtest filter. We do this so that we can selectively run tests that
  // require root or specific OS support (e.g., cgroups). Note that this
  // should not effect any other filters that have been put in place
  // either on the command line or via an environment variable.
  // NOTE: This should be done before invoking `RUN_ALL_TESTS`.
  Environment(vector<std::shared_ptr<TestFilter>> filters)
  {
    // First we split the current filter into enabled and disabled tests
    // (which are separated by a '-').
    const string& filter = ::testing::GTEST_FLAG(filter);

    // An empty filter indicates no tests should be run.
    if (filter.empty()) {
      return;
    }

    filters.push_back(std::shared_ptr<TestFilter>(new SymlinkFilter()));

    string enabled;
    string disabled;

    size_t dash = filter.find('-');
    if (dash != string::npos) {
      enabled = filter.substr(0, dash);
      disabled = filter.substr(dash + 1);
    } else {
      enabled = filter;
    }

    // Use universal filter if not specified.
    if (enabled.empty()) {
      enabled = "*";
    }

    // Ensure disabled tests end with ":" separator before we add more.
    if (!disabled.empty() && !strings::endsWith(disabled, ":")) {
      disabled += ":";
    }

    // Construct the filter string to handle system or platform specific tests.
    ::testing::UnitTest* unitTest = ::testing::UnitTest::GetInstance();

    disabled += strings::join(":", tests::disabled(unitTest, filters));

    // Now update the gtest flag.
    ::testing::GTEST_FLAG(filter) = enabled + "-" + disabled;
  }
};

} // namespace tests {
} // namespace internal {
} // namespace stout {

#endif // __STOUT_TESTS_ENVIRONMENT_HPP__
