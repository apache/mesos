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

#ifndef __WINDOWS__
#include <sys/wait.h>
#endif // __WINDOWS__

#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

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
#include <stout/os/pstree.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/temp.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#include "linux/perf.hpp"
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

using stout::internal::tests::TestFilter;

namespace mesos {
namespace internal {
namespace tests {

// Storage for the global environment instance.
Environment* environment;


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
        << cfsError->message << "\n"
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
    Try<set<string>> hierarchies = cgroups::hierarchies();
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
    } else if (!hierarchies->empty()) {
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
        << dockerError->message << "\n"
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
        << "Cannot enable net_cls cgroup subsystem associated test cases \n"
        << "since we cannot determine the existence of the net_cls cgroup\n"
        << "subsystem.\n"
        << "-------------------------------------------------------------\n";
      return;
    }

    if (netCls.isSome() && !netCls.get()) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "Cannot enable net_cls cgroup subsystem associated test cases \n"
        << "since net_cls cgroup subsystem is not enabled. Check instructions\n"
        << "for your linux distrubtion to enable the net_cls cgroup subsystem\n"
        << "on your system.\n"
        << "-----------------------------------------------------------\n";
      return;
    }
    netClsError = false;
#else
    std::cerr
        << "-----------------------------------------------------------\n"
        << "Cannot enable net_cls cgroup subsystem associated test cases\n"
        << "since this platform does not support cgroups.\n"
        << "-----------------------------------------------------------\n";
#endif
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "NET_CLS_") && netClsError;
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
  explicit SupportedFilesystemTestFilter(const string& fsname)
  {
#ifdef __linux__
    Try<bool> check = fs::supported(fsname);
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
        << fsSupportError->message << "\n"
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
    bool perfUnavailable = !perf::supported();
    if (perfUnavailable) {
      perfError = Error(
          "Could not find the 'perf' command or its version lower that "
          "2.6.39 so tests using it to sample the 'cpu-cycles' hardware "
          "event will not be run.");
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
        << perfError->message << "\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    // Disable all tests that try to sample 'cpu-cycles' events using 'perf'.
    return (matches(test, "ROOT_CGROUPS_PERF_PerfTest") ||
            matches(test, "ROOT_CGROUPS_PERF_UserCgroup") ||
            matches(test, "ROOT_CGROUPS_PERF_RollForward") ||
            matches(test, "ROOT_CGROUPS_PERF_Sample")) && perfError.isSome();
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
    perfError = !perf::supported();
    if (perfError) {
      std::cerr
        << "-------------------------------------------------------------\n"
        << "require 'perf' version >= 2.6.39 so no 'perf' tests will be run\n"
        << "-------------------------------------------------------------"
        << std::endl;
    }
#else
    perfError = true;
#endif // __linux__
  }

  bool disable(const ::testing::TestInfo* test) const
  {
    return matches(test, "PERF_") && perfError;
  }

private:
  bool perfError;
};


class RootFilter : public TestFilter
{
public:
  bool disable(const ::testing::TestInfo* test) const
  {
#ifdef __WINDOWS__
    // On Windows, `root` does not exist, so we cannot run `ROOT_` tests.
    return matches(test, "ROOT_");
#else
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
#endif // __WINDOWS__
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


Environment::Environment(const Flags& _flags)
  : stout::internal::tests::Environment(
        std::vector<std::shared_ptr<TestFilter>>{
            std::make_shared<AufsFilter>(),
            std::make_shared<BenchmarkFilter>(),
            std::make_shared<CfsFilter>(),
            std::make_shared<CgroupsFilter>(),
            std::make_shared<CurlFilter>(),
            std::make_shared<DockerFilter>(),
            std::make_shared<InternetFilter>(),
            std::make_shared<LogrotateFilter>(),
            std::make_shared<NetcatFilter>(),
            std::make_shared<NetClsCgroupsFilter>(),
            std::make_shared<NetworkIsolatorTestFilter>(),
            std::make_shared<NvidiaGpuFilter>(),
            std::make_shared<OverlayFSFilter>(),
            std::make_shared<PerfCPUCyclesFilter>(),
            std::make_shared<PerfFilter>(),
            std::make_shared<RootFilter>(),
            std::make_shared<UnzipFilter>(),
            std::make_shared<XfsFilter>()}),
    flags(_flags)
{
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
  foreachkey (const string& key, os::environment()) {
    if (key.find("MESOS_") == 0) {
      os::unsetenv(key);
    }
  }

  // Set the path to the native JNI library for running JVM tests.
  // TODO(tillt): Adapt library towards JNI specific name once libmesos
  // has been split.
  if (os::getenv("MESOS_NATIVE_JAVA_LIBRARY").isNone()) {
    string path = getLibMesosPath();
    os::setenv("MESOS_NATIVE_JAVA_LIBRARY", path);
  }

  // TODO(hausdorff): Revisit whether we need this check when we complete work
  // to light up Agent tests on Windows (see epic tracking this work at
  // MESOS-6695). As we incrementally add tests to the Windows build, we will
  // add this check to the tests that need it; eventually, the goal is to get
  // rid of this altogether. See MESOS-5903.
#ifndef __WINDOWS__
  if (!GTEST_IS_THREADSAFE) {
    EXIT(EXIT_FAILURE) << "Testing environment is not thread safe, bailing!";
  }
#endif // __WINDOWS__
}


void Environment::TearDown()
{
  // Make sure we haven't left any child processes lying around.
  // TODO(benh): Look for processes in the same group or session that
  // might have been reparented.
  // TODO(jmlvanre): Consider doing this `OnTestEnd` in a listener so
  // that we can identify leaked processes more precisely.
  Try<os::ProcessTree> pstree = os::pstree(0);

  if (pstree.isSome() && !pstree->children.empty()) {
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
      Try<Nothing> unmount = fs::unmountAll(directory, MNT_DETACH);
      if (unmount.isError()) {
        LOG(ERROR) << "Failed to umount for directory '" << directory
                   << "': " << unmount.error();
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

  const string tmpdir = os::temp();

  const string& path =
#ifndef __WINDOWS__
    path::join(tmpdir, strings::join("_", testCase, testName, "XXXXXX"));
#else
    // TODO(hausdorff): When we resolve MESOS-5849, we should change
    // this back to the same path as the Unix version. This is
    // currently necessary to make the sandbox path short enough to
    // avoid the infamous Windows path length errors, which would
    // normally cause many of our tests to fail.
    path::join(tmpdir, "XXXXXX");
#endif // __WINDOWS__

  Try<string> mkdtemp = os::mkdtemp(path);
  if (mkdtemp.isSome()) {
    directories.push_back(mkdtemp.get());
  }
  return mkdtemp;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
