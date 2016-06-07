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

#include <sys/wait.h>

#include <string.h>
#include <unistd.h>

#include <list>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "docker/docker.hpp"

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/shell.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#endif

#ifdef WITH_NETWORK_ISOLATOR
#include "linux/routing/utils.hpp"
#endif

#include "logging/logging.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/utils.hpp"

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
    } else if (test->type_param() != nullptr &&
               strings::contains(test->type_param(), pattern)) {
      return true;
    }

    return false;
  }
};


class BenchmarkFilter : public TestFilter
{
public:
  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "BENCHMARK_") && !flags.benchmark;
  }
};


class CfsFilter : public TestFilter
{
public:
  CfsFilter()
  {
#ifdef __linux__
    Result<string> hierarchy = cgroups::hierarchy("cpu");
    if (hierarchy.isSome()) {
      bool cfsQuotaEnabled =
        os::exists(path::join(hierarchy.get(), "cpu.cfs_quota_us"));

      if (cfsQuotaEnabled) {
        cfsError = None();
      } else {
        cfsError = Error("CFS bandwidth control is not available");
      }
    } else if (hierarchy.isError()) {
      cfsError = Error(
          "There was an error finding the 'cpu' cgroup hierarchy:\n" +
          hierarchy.error());
    } else {
      cfsError = Error(
          "The 'cpu' cgroup hierarchy was not found, which means\n"
          "that CFS bandwidth control is not available");
    }

    if (cfsError.isSome()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "The 'CFS_' tests cannot be run because:\n"
        << cfsError.get().message << "\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
#else
    cfsError = Error(
        "These tests require CFS bandwidth control, which is a "
        "Linux kernel feature, but Linux has not been detected");
#endif // __linux__
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "CFS_") && cfsError.isSome();
  }

private:
  Option<Error> cfsError;
};


class CgroupsFilter : public TestFilter
{
public:
  CgroupsFilter()
  {
#ifdef __linux__
    Try<set<string> > hierarchies = cgroups::hierarchies();
    if (hierarchies.isError()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any cgroups tests that require mounting\n"
        << "hierarchies because reading cgroup heirarchies failed:\n"
        << hierarchies.error() << "\n"
        << "We'll disable the CgroupsNoHierarchyTest test fixture for now.\n"
        << "-------------------------------------------------------------"
        << std::endl;

      error = hierarchies.error();
    } else if (!hierarchies.get().empty()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any cgroups tests that require mounting\n"
        << "hierarchies because you have the following hierarchies mounted:\n"
        << strings::trim(stringify(hierarchies.get()), " {},") << "\n"
        << "We'll disable the CgroupsNoHierarchyTest test fixture for now.\n"
        << "-------------------------------------------------------------"
        << std::endl;

      error = Error("Hierarchies exist");
    }
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
               error.isSome();
      }

      return user.get() != "root" || !cgroups::enabled();
#else
      return true;
#endif // __linux__
    }

    return false;
  }

private:
  Option<Error> error;
};


class CurlFilter : public TestFilter
{
public:
  CurlFilter()
  {
    curlError = os::system("which curl") != 0;
    if (curlError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'curl' command found so no 'curl' tests will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "CURL_") && curlError;
  }

private:
  bool curlError;
};


class NvidiaGpuFilter : public TestFilter
{
public:
  NvidiaGpuFilter()
  {
    exists = os::system("which nvidia-smi") == 0;
    if (!exists) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'nvidia-smi' command found so no Nvidia GPU tests will run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "NVIDIA_GPU_") && !exists;
  }

private:
  bool exists;
};


class DockerFilter : public TestFilter
{
public:
  DockerFilter()
  {
#ifdef __linux__
    Try<Owned<Docker>> docker = Docker::create(
        flags.docker,
        flags.docker_socket);

    if (docker.isError()) {
      dockerError = docker.error();
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


class InternetFilter : public TestFilter
{
public:
  InternetFilter()
  {
    error = os::system("ping -c 1 -W 1 google.com") != 0;
    if (error) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any INTERNET tests because no internet access\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "INTERNET_") && error;
  }

private:
  bool error;
};


class LogrotateFilter : public TestFilter
{
public:
  LogrotateFilter()
  {
    logrotateError = os::system("which logrotate") != 0;
    if (logrotateError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'logrotate' command found so no 'logrotate' tests\n"
        << "will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "LOGROTATE_") && logrotateError;
  }

private:
  bool logrotateError;
};


class NetcatFilter : public TestFilter
{
public:
  NetcatFilter()
  {
    netcatError = os::system("which nc") != 0;
    if (netcatError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'nc' command found so no tests depending on 'nc' will run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "NC_") && netcatError;
  }

private:
  bool netcatError;
};


// This filter enables tests for the cgroups/net_cls isolator after
// checking that net_cls cgroup subsystem has been enabled on the
// system. We cannot rely on the generic cgroups test filter
// ('CgroupsFilter') to enable the cgroups/net_cls isolator tests
// since, although cgroups might be enabled on a linux distribution
// the net_cls subsystem is not turned on, by default, on all
// distributions.
class NetClsCgroupsFilter : public TestFilter
{
public:
  NetClsCgroupsFilter()
  {
    netClsError = true;
#ifdef __linux__
    Try<bool> netCls = cgroups::enabled("net_cls");

    if (netCls.isError()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "Cannot enable NetClsIsolatorTest since we cannot determine \n"
        << "the existence of the net_cls cgroup subsystem.\n"
        << "-------------------------------------------------------------\n";
      return;
    }

    if (netCls.isSome() && !netCls.get()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "Cannot enable NetClsIsolatorTest since net_cls cgroup \n"
        << "subsystem is not enabled. Check instructions for your linux\n"
        << "distrubtion to enable the net_cls cgroups on your system.\n"
        << "-----------------------------------------------------------\n";
      return;
    }
    netClsError = false;
#else
    std::cerr
        << "-----------------------------------------------------------\n"
        << "Cannot enable NetClsIsolatorTest since this platform does\n"
        << "not support cgroups.\n"
        << "-----------------------------------------------------------\n";
#endif
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    if (matches(test, "NetClsIsolatorTest")) {
      return netClsError;
    }
    return false;
  }

private:
  bool netClsError;
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


class SupportedFilesystemTestFilter : public TestFilter
{
public:
  explicit SupportedFilesystemTestFilter(const string fsname)
  {
#ifdef __linux__
    Try<bool> check = (fsname == "overlayfs")
      ? fs::overlay::supported()
      : fs::supported(fsname);

    if (check.isError()) {
      fsSupportError = check.error();
    } else if (!check.get()) {
      fsSupportError = Error(fsname + " is not supported on your systems");
    }
#else
    fsSupportError =
      Error(fsname + " tests not supported on non-Linux systems");
#endif

    if (fsSupportError.isSome()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "We cannot run any " << fsname << " tests because:\n"
        << fsSupportError.get().message << "\n"
        << "-------------------------------------------------------------\n";
    }
  }

  Option<Error> fsSupportError;
};


class AufsFilter : public SupportedFilesystemTestFilter
{
public:
  AufsFilter() : SupportedFilesystemTestFilter("aufs") {}

  bool disable(const ::testing::TestInfo* test) const
  {
    return fsSupportError.isSome() && matches(test, "AUFS_");
  }
};


class OverlayFSFilter : public SupportedFilesystemTestFilter
{
public:
  OverlayFSFilter() : SupportedFilesystemTestFilter("overlayfs") {}

  bool disable(const ::testing::TestInfo* test) const
  {
    return fsSupportError.isSome() && matches(test, "OVERLAYFS_");
  }
};


class XfsFilter : public SupportedFilesystemTestFilter
{
public:
  XfsFilter() : SupportedFilesystemTestFilter("xfs") {}

  bool disable(const ::testing::TestInfo* test) const
  {
    return fsSupportError.isSome() && matches(test, "XFS_");
  }
};


class PerfCPUCyclesFilter : public TestFilter
{
public:
  PerfCPUCyclesFilter()
  {
#ifdef __linux__
    bool perfUnavailable = os::system("perf help >&-") != 0;
    if (perfUnavailable) {
      perfError = Error(
          "The 'perf' command wasn't found so tests using it\n"
          "to sample the 'cpu-cycles' hardware event will not be run.");
    } else {
      bool cyclesUnavailable =
        os::system("perf list hw | grep cpu-cycles >/dev/null") != 0;
      if (cyclesUnavailable) {
        perfError = Error(
            "The 'cpu-cycles' hardware event of 'perf' is not available on\n"
            "this platform so tests using it will not be run.\n"
            "One likely reason is that the tests are run in a virtual\n"
            "machine that does not provide CPU performance counters");
      }
    }
#else
    perfError = Error("Tests using 'perf' cannot be run on non-Linux systems");
#endif // __linux__

    if (perfError.isSome()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << perfError.get().message << "\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    // Disable all tests that try to sample 'cpu-cycles' events using 'perf'.
    return (matches(test, "ROOT_CGROUPS_Perf") ||
            matches(test, "ROOT_CGROUPS_Sample") ||
            matches(test, "ROOT_CGROUPS_UserCgroup") ||
            matches(test, "CGROUPS_ROOT_PerfRollForward") ||
            matches(test, "ROOT_Sample")) && perfError.isSome();
  }

private:
  Option<Error> perfError;
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


class UnzipFilter : public TestFilter
{
public:
  UnzipFilter()
  {
    unzipError = os::system("which unzip") != 0;
    if (unzipError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "No 'unzip' command found so no 'unzip' tests will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "UNZIP_") && unzipError;
  }

private:
  bool unzipError;
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

  filters.push_back(Owned<TestFilter>(new AufsFilter()));
  filters.push_back(Owned<TestFilter>(new BenchmarkFilter()));
  filters.push_back(Owned<TestFilter>(new CfsFilter()));
  filters.push_back(Owned<TestFilter>(new CgroupsFilter()));
  filters.push_back(Owned<TestFilter>(new CurlFilter()));
  filters.push_back(Owned<TestFilter>(new DockerFilter()));
  filters.push_back(Owned<TestFilter>(new InternetFilter()));
  filters.push_back(Owned<TestFilter>(new LogrotateFilter()));
  filters.push_back(Owned<TestFilter>(new NetcatFilter()));
  filters.push_back(Owned<TestFilter>(new NetClsCgroupsFilter()));
  filters.push_back(Owned<TestFilter>(new NetworkIsolatorTestFilter()));
  filters.push_back(Owned<TestFilter>(new NvidiaGpuFilter()));
  filters.push_back(Owned<TestFilter>(new OverlayFSFilter()));
  filters.push_back(Owned<TestFilter>(new PerfCPUCyclesFilter()));
  filters.push_back(Owned<TestFilter>(new PerfFilter()));
  filters.push_back(Owned<TestFilter>(new RootFilter()));
  filters.push_back(Owned<TestFilter>(new UnzipFilter()));
  filters.push_back(Owned<TestFilter>(new XfsFilter()));

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

  // Add the temporary directory event listener which will clean up
  // any directories created after each test finishes.
  temporaryDirectoryEventListener = new TemporaryDirectoryEventListener();
  listeners.Append(temporaryDirectoryEventListener);
}


void Environment::SetUp()
{
  // Clear any MESOS_ environment variables so they don't affect our tests.
  char** environ = os::raw::environment();
  for (int i = 0; environ[i] != nullptr; i++) {
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
    string path = getLibMesosPath();
    os::setenv("MESOS_NATIVE_JAVA_LIBRARY", path);
  }

  if (!GTEST_IS_THREADSAFE) {
    EXIT(EXIT_FAILURE) << "Testing environment is not thread safe, bailing!";
  }
}


void Environment::TearDown()
{
  // Make sure we haven't left any child processes lying around.
  // TODO(benh): Look for processes in the same group or session that
  // might have been reparented.
  // TODO(jmlvanre): Consider doing this `OnTestEnd` in a listener so
  // that we can identify leaked processes more precisely.
  Try<os::ProcessTree> pstree = os::pstree(0);

  if (pstree.isSome() && !pstree.get().children.empty()) {
    FAIL() << "Tests completed with child processes remaining:\n"
           << pstree.get();
  }
}


Try<string> Environment::mkdtemp()
{
  return temporaryDirectoryEventListener->mkdtemp();
}


void tests::Environment::TemporaryDirectoryEventListener::OnTestEnd(
    const testing::TestInfo&)
{
  foreach (const string& directory, directories) {
#ifdef __linux__
    // Try to remove any mounts under 'directory'.
    if (::geteuid() == 0) {
      Try<string> umount = os::shell(
          "grep '%s' /proc/mounts | "
          "cut -d' ' -f2 | "
          "xargs --no-run-if-empty umount -l",
          directory.c_str());

      if (umount.isError()) {
        LOG(ERROR) << "Failed to umount for directory '" << directory
                   << "': " << umount.error();
      }
    }
#endif

    Try<Nothing> rmdir = os::rmdir(directory);
    if (rmdir.isError()) {
      LOG(ERROR) << "Failed to remove '" << directory
                 << "': " << rmdir.error();
    }
  }

  directories.clear();
}


Try<string> Environment::TemporaryDirectoryEventListener::mkdtemp()
{
  const ::testing::TestInfo* const testInfo =
    ::testing::UnitTest::GetInstance()->current_test_info();

  if (testInfo == nullptr) {
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

  Option<string> tmpdir = os::getenv("TMPDIR");

  if (tmpdir.isNone()) {
    tmpdir = "/tmp";
  }

  const string& path =
    path::join(tmpdir.get(), strings::join("_", testCase, testName, "XXXXXX"));

  Try<string> mkdtemp = os::mkdtemp(path);
  if (mkdtemp.isSome()) {
    directories.push_back(mkdtemp.get());
  }
  return mkdtemp;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
