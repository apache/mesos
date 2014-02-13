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

#include <sys/wait.h>

#include <string.h>

#include <list>
#include <string>

#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "logging/logging.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

// Storage for the global environment instance.
Environment* environment;


// Returns true if we should enable the provided test. Similar to how
// tests can be disabled using the 'DISABLED_' prefix on a test case
// name or test name, we use:
//
//   'ROOT_' : Disable test if current user isn't root.
//   'CGROUPS_' : Disable test if cgroups support isn't present.
//   'NOHIERARCHY_' : Disable test if there is already a cgroups
//       hierarchy mounted.
//
// These flags can be composed in any order, but must come after
// 'DISABLED_'. In addition, we disable tests that attempt to use the
// CgroupsIsolator type parameter if the current user is not root or
// cgroups is not supported.
// TODO(benh): Provide a generic way to enable/disable tests by
// registering "filter" functions.
static bool enable(const ::testing::TestInfo& test)
{
  // First check the test case name and test name.
  list<string> names;
  names.push_back(test.test_case_name());
  names.push_back(test.name());

  foreach (const string& name, names) {
    if (strings::contains(name, "ROOT_") && os::user() != "root") {
      return false;
    }

    if (strings::contains(name, "CGROUPS_") && !os::exists("/proc/cgroups")) {
      return false;
    }

#ifdef __linux__
    if (strings::contains(name, "NOHIERARCHY_")) {
      Try<std::set<std::string> > hierarchies = cgroups::hierarchies();
      CHECK_SOME(hierarchies);
      if (!hierarchies.get().empty()) {
        std::cerr
          << "-------------------------------------------------------------\n"
          << "We cannot run any cgroups tests that require mounting\n"
          << "hierarchies because you have the following hierarchies mounted:\n"
          << strings::trim(stringify(hierarchies.get()), " {},") << "\n"
          << "We'll disable the CgroupsNoHierarchyTest test fixture for now.\n"
          << "-------------------------------------------------------------"
          << std::endl;

        return false;
      }
    }

    // On Linux non-privileged users are limited to 64k of locked memory so we
    // cannot run the MemIsolatorTest.Usage.
    if (strings::contains(name, "MemIsolatorTest") && os::user() != "root") {
      return false;
    }
#endif
  }

  // Now check the type parameter.
  if (test.type_param() != NULL) {
    const string& type = test.type_param();
    if (strings::contains(type, "Cgroups") &&
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
  // First we split the current filter into enabled and disabled tests
  // (which are separated by a '-').
  const string& filter = ::testing::GTEST_FLAG(filter);

  // An empty filter indicates no tests should be run.
  if (filter.empty()) {
    return;
  }

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
  for (int i = 0; i < unitTest->total_test_case_count(); i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);
    for (int j = 0; j < testCase->total_test_count(); j++) {
      const ::testing::TestInfo* testInfo = testCase->GetTestInfo(j);

      if (!enable(*testCase->GetTestInfo(j))) {
        // Append 'TestCase.TestName:'.
        disabled.append(testInfo->test_case_name());
        disabled.append(".");
        disabled.append(testInfo->name());
        disabled.append(":");
      }
    }
  }

  // Now update the gtest flag.
  ::testing::GTEST_FLAG(filter) = enabled + "-" + disabled;

  // Add our test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::FilterTestEventListener::instance());
  listeners.Append(process::ClockTestEventListener::instance());
}


void Environment::SetUp()
{
  // Clear any MESOS_ environment variables so they don't affect our tests.
  char** environ = os::environ();
  for (int i = 0; environ[i] != NULL; i++) {
    std::string variable = environ[i];
    if (variable.find("MESOS_") == 0) {
      string key;
      size_t eq = variable.find_first_of("=");
      if (eq == string::npos) {
        continue; // Not expecting a missing '=', but ignore anyway.
      }
      os::unsetenv(variable.substr(strlen("MESOS_"), eq - strlen("MESOS_")));
    }
  }

  // Set the path to the native library for running JVM tests.
  if (!os::hasenv("MESOS_NATIVE_LIBRARY")) {
    string path = path::join(tests::flags.build_dir, "src", ".libs");
#ifdef __APPLE__
    path = path::join(path, "libmesos-" VERSION ".dylib");
#else
    path = path::join(path, "libmesos-" VERSION ".so");
#endif
    os::setenv("MESOS_NATIVE_LIBRARY", path);
  }

  if (!GTEST_IS_THREADSAFE) {
    EXIT(1) << "Testing environment is not thread safe, bailing!";
  }
}


void Environment::TearDown()
{
  foreach (const string& directory, directories) {
    Try<Nothing> rmdir = os::rmdir(directory);
    if (rmdir.isError()) {
      LOG(ERROR) << "Failed to remove '" << directory
                 << "': " << rmdir.error();
    }
  }
  directories.clear();

  // Make sure we haven't left any child processes lying around.
  Try<os::ProcessTree> pstree = os::pstree(0);

  if (pstree.isSome() && !pstree.get().children.empty()) {
    FAIL() << "Tests completed with child processes remaining:\n"
           << pstree.get();
  }
}


Try<string> Environment::mkdtemp()
{
  const ::testing::TestInfo* const testInfo =
    ::testing::UnitTest::GetInstance()->current_test_info();

  if (testInfo == NULL) {
    return Error("Failed to determine the current test information");
  }

  // We replace any slashes present in the test names (e.g. TYPED_TEST),
  // to make sure the temporary directory resides under '/tmp/'.
  const string& testCase =
    strings::replace(testInfo->test_case_name(), "/", "_");

  string testName = strings::replace(testInfo->name(), "/", "_");

  // Adjust the test name to remove any 'DISABLED_' prefix (to make
  // things easier to read). While this might seem alarming, if we are
  // "running" a disabled test it must be the case that the test was
  // explicitly enabled (e.g., via 'gtest_filter').
  if (strings::startsWith(testName, "DISABLED_")) {
    testName = strings::remove(testName, "DISABLED_", strings::PREFIX);
  }

  const string& path =
    path::join("/tmp", strings::join("_", testCase, testName, "XXXXXX"));

  Try<string> mkdtemp = os::mkdtemp(path);
  if (mkdtemp.isSome()) {
    directories.push_back(mkdtemp.get());
  }
  return mkdtemp;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

