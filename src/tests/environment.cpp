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
#include <set>
#include <string>
#include <vector>

#include "docker/docker.hpp"

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#ifdef WITH_NETWORK_ISOLATOR
#include "linux/routing/utils.hpp"
#endif

#include "logging/logging.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"

#ifdef WITH_NETWORK_ISOLATOR
using namespace routing;
#endif

using std::list;
using std::set;
using std::string;
using std::vector;

using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

// Storage for the global environment instance.
Environment* environment;


class TestFilter
{
public:
  TestFilter() {}
  virtual ~TestFilter() {}

  // Returns true iff the test should be disabled.
  virtual bool disable(const ::testing::TestInfo* test) const = 0;

  // Returns whether the test name / parameterization matches the
  // pattern.
  static bool matches(const ::testing::TestInfo* test, const string& pattern)
  {
    if (strings::contains(test->test_case_name(), pattern) ||
        strings::contains(test->name(), pattern)) {
      return true;
    } else if (test->type_param() != NULL &&
               strings::contains(test->type_param(), pattern)) {
      return true;
    }

    return false;
  }
};


class RootFilter : public TestFilter
{
public:
  bool disable(const ::testing::TestInfo* test) const
  {
    Result<string> user = os::user();
    CHECK_SOME(user);

#ifdef __linux__
    // On Linux non-privileged users are limited to 64k of locked
    // memory so we cannot run the MemIsolatorTest.Usage.
    if (matches(test, "MemIsolatorTest")) {
      return user.get() != "root";
    }
#endif // __linux__

    return matches(test, "ROOT_") && user.get() != "root";
  }
};


class CgroupsFilter : public TestFilter
{
public:
  CgroupsFilter()
  {
#ifdef __linux__
    Try<set<string> > hierarchies_ = cgroups::hierarchies();
    CHECK_SOME(hierarchies_);
    if (!hierarchies_.get().empty()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any cgroups tests that require mounting\n"
        << "hierarchies because you have the following hierarchies mounted:\n"
        << strings::trim(stringify(hierarchies_.get()), " {},") << "\n"
        << "We'll disable the CgroupsNoHierarchyTest test fixture for now.\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }

    hierarchies = hierarchies_.get();
#endif // __linux__
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    if (matches(test, "CGROUPS_") || matches(test, "Cgroups")) {
#ifdef __linux__
      Result<string> user = os::user();
      CHECK_SOME(user);

      if (matches(test, "NOHIERARCHY_")) {
        return user.get() != "root" ||
               !cgroups::enabled() ||
               !hierarchies.empty();
      }

      return user.get() != "root" || !cgroups::enabled();
#else
      return true;
#endif // __linux__
    }

    return false;
  }

private:
  set<string> hierarchies;
};


class DockerFilter : public TestFilter
{
public:
  DockerFilter()
  {
#ifdef __linux__
    Try<Docker*> docker = Docker::create(flags.docker);
    if (docker.isError()) {
      dockerError = docker.error();
    } else {
      delete docker.get();
    }
#else
    dockerError = Error("Docker tests not supported on non-Linux systems");
#endif // __linux__

    if (dockerError.isSome()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any Docker tests because:\n"
        << dockerError.get().message << "\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "DOCKER_") && dockerError.isSome();
  }

private:
  Option<Error> dockerError;
};


class PerfFilter : public TestFilter
{
public:
  PerfFilter()
  {
#ifdef __linux__
    perfError = os::system("perf help >&-") != 0;
    if (perfError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'perf' command found so no 'perf' tests will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
#else
      perfError = true;
#endif // __linux__
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    // Currently all tests that require 'perf' are part of the
    // 'PerfTest' test fixture, hence we check for 'Perf' here.
    //
    // TODO(ijimenez): Replace all tests which require 'perf' with
    // the prefix 'PERF_' to be more consistent with the filter
    // naming we've done (i.e., ROOT_, CGROUPS_, etc).
    return matches(test, "Perf") && perfError;
  }

private:
  bool perfError;
};


class BenchmarkFilter : public TestFilter
{
public:
  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "BENCHMARK_") && !flags.benchmark;
  }
};


class NetworkIsolatorTestFilter : public TestFilter
{
public:
  NetworkIsolatorTestFilter()
  {
#ifdef WITH_NETWORK_ISOLATOR
    Try<Nothing> check = routing::check();
    if (check.isError()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any PortMapping tests because:\n"
        << check.error() << "\n"
        << "-------------------------------------------------------------\n";

      portMappingError = Error(check.error());
    }
#endif
  }

  bool disable(const ::testing::TestInfo* test) const
  {
#ifdef WITH_NETWORK_ISOLATOR
    // PortMappingIsolatorProcess doesn't suport test
    // 'SlaveRecoveryTest.MultipleSlaves'.
    if (matches(test, "SlaveRecoveryTest") &&
        matches(test, "MultipleSlaves")) {
      return true;
    }
#endif

    if (matches(test, "PortMappingIsolatorTest") ||
        matches(test, "PortMappingMesosTest")) {
#ifdef WITH_NETWORK_ISOLATOR
      return !portMappingError.isNone();
#else
      return true;
#endif
    }

    return false;
  }

private:
  Option<Error> portMappingError;
};


// Return list of disabled tests based on test name based filters.
static vector<string> disabled(
    const ::testing::UnitTest* unitTest,
    const vector<Owned<TestFilter> >& filters)
{
  vector<string> disabled;

  for (int i = 0; i < unitTest->total_test_case_count(); i++) {
    const ::testing::TestCase* testCase = unitTest->GetTestCase(i);
    for (int j = 0; j < testCase->total_test_count(); j++) {
      const ::testing::TestInfo* test = testCase->GetTestInfo(j);

      foreach (const Owned<TestFilter>& filter, filters) {
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


// We use the constructor to setup specific tests by updating the
// gtest filter. We do this so that we can selectively run tests that
// require root or specific OS support (e.g., cgroups). Note that this
// should not effect any other filters that have been put in place
// either on the command line or via an environment variable.
// N.B. This MUST be done _before_ invoking RUN_ALL_TESTS.
Environment::Environment(const Flags& _flags) : flags(_flags)
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

  vector<Owned<TestFilter> > filters;

  filters.push_back(Owned<TestFilter>(new RootFilter()));
  filters.push_back(Owned<TestFilter>(new CgroupsFilter()));
  filters.push_back(Owned<TestFilter>(new DockerFilter()));
  filters.push_back(Owned<TestFilter>(new BenchmarkFilter()));
  filters.push_back(Owned<TestFilter>(new NetworkIsolatorTestFilter()));
  filters.push_back(Owned<TestFilter>(new PerfFilter()));

  // Construct the filter string to handle system or platform specific tests.
  ::testing::UnitTest* unitTest = ::testing::UnitTest::GetInstance();

  disabled += strings::join(":", tests::disabled(unitTest, filters));

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
    string variable = environ[i];
    if (variable.find("MESOS_") == 0) {
      string key;
      size_t eq = variable.find_first_of("=");
      if (eq == string::npos) {
        continue; // Not expecting a missing '=', but ignore anyway.
      }
      os::unsetenv(variable.substr(strlen("MESOS_"), eq - strlen("MESOS_")));
    }
  }

  // Set the path to the native JNI library for running JVM tests.
  // TODO(tillt): Adapt library towards JNI specific name once libmesos
  // has been split.
  if (os::getenv("MESOS_NATIVE_JAVA_LIBRARY").isNone()) {
    string path = path::join(tests::flags.build_dir, "src", ".libs");
#ifdef __APPLE__
    path = path::join(path, "libmesos-" VERSION ".dylib");
#else
    path = path::join(path, "libmesos-" VERSION ".so");
#endif
    os::setenv("MESOS_NATIVE_JAVA_LIBRARY", path);
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
  // TODO(benh): Look for processes in the same group or session that
  // might have been reparented.
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
