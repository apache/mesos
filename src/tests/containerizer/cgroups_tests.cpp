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

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gmock/gmock.h>

#include <process/gtest.hpp>
#include <process/latch.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/exit.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/pagesize.hpp>

#include "linux/cgroups.hpp"
#include "linux/perf.hpp"

#include "tests/mesos.hpp" // For TEST_CGROUPS_(HIERARCHY|ROOT).
#include "tests/utils.hpp"

#include "tests/containerizer/memory_test_helper.hpp"

using namespace process;

using cgroups::memory::pressure::Level;
using cgroups::memory::pressure::Counter;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


class CgroupsTest : public TemporaryDirectoryTest
{
public:
  static void SetUpTestCase()
  {
    // Clean up the testing hierarchy, in case it wasn't cleaned up
    // properly from previous tests.
    AWAIT_READY(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
  }

  static void TearDownTestCase()
  {
    AWAIT_READY(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
  }
};


// A fixture which is used to name tests that expect NO hierarchy to
// exist in order to test the ability to create a hierarchy (since
// most likely existing hierarchies will have all or most subsystems
// attached rendering our ability to create a hierarchy fruitless).
class CgroupsNoHierarchyTest : public CgroupsTest
{
public:
  static void SetUpTestCase()
  {
    CgroupsTest::SetUpTestCase();

    Try<set<string>> hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    ASSERT_TRUE(hierarchies->empty())
      << "-------------------------------------------------------------\n"
      << "We cannot run any cgroups tests that require mounting\n"
      << "hierarchies because you have the following hierarchies mounted:\n"
      << strings::trim(stringify(hierarchies.get()), " {},") << "\n"
      << "You can either unmount those hierarchies, or disable\n"
      << "this test case (i.e., --gtest_filter=-CgroupsNoHierarchyTest.*).\n"
      << "-------------------------------------------------------------";
  }
};


// A fixture that assumes ANY hierarchy is acceptable for use provided
// it has the subsystems attached that were specified in the
// constructor. If no hierarchy could be found that has all the
// required subsystems then we attempt to create a new hierarchy.
class CgroupsAnyHierarchyTest : public CgroupsTest
{
public:
  CgroupsAnyHierarchyTest(const string& _subsystems = "cpu")
    : subsystems(_subsystems) {}

protected:
  void SetUp() override
  {
    CgroupsTest::SetUp();

    foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
      // Establish the base hierarchy if this is the first subsystem checked.
      if (baseHierarchy.empty()) {
        Result<string> hierarchy = cgroups::hierarchy(subsystem);
        ASSERT_FALSE(hierarchy.isError());

        if (hierarchy.isNone()) {
          baseHierarchy = TEST_CGROUPS_HIERARCHY;
        } else {
          // Strip the subsystem to get the base hierarchy.
          Try<string> baseDirname = Path(hierarchy.get()).dirname();
          ASSERT_SOME(baseDirname);
          baseHierarchy = baseDirname.get();
        }
      }

      // Mount the subsystem if necessary.
      string hierarchy = path::join(baseHierarchy, subsystem);
      Try<bool> mounted = cgroups::mounted(hierarchy, subsystem);
      ASSERT_SOME(mounted);
      if (!mounted.get()) {
        ASSERT_SOME(cgroups::mount(hierarchy, subsystem))
          << "-------------------------------------------------------------\n"
          << "We cannot run any cgroups tests that require\n"
          << "a hierarchy with subsystem '" << subsystem << "'\n"
          << "because we failed to find an existing hierarchy\n"
          << "or create a new one (tried '" << hierarchy << "').\n"
          << "You can either remove all existing\n"
          << "hierarchies, or disable this test case\n"
          << "(i.e., --gtest_filter=-"
          << ::testing::UnitTest::GetInstance()
              ->current_test_info()
              ->test_case_name() << ".*).\n"
          << "-------------------------------------------------------------";
      }

      Try<vector<string>> cgroups = cgroups::get(hierarchy);
      ASSERT_SOME(cgroups);

      foreach (const string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT.
        if (cgroup == TEST_CGROUPS_ROOT) {
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));
        }
      }
    }
  }

  void TearDown() override
  {
    // Remove all *our* cgroups.
    foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
      string hierarchy = path::join(baseHierarchy, subsystem);

      Try<vector<string>> cgroups = cgroups::get(hierarchy);
      ASSERT_SOME(cgroups);

      foreach (const string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT.
        if (cgroup == TEST_CGROUPS_ROOT) {
          // Since we are tearing down the tests, kill any processes
          // that might remain. Any remaining zombie processes will
          // not prevent the destroy from succeeding.
          EXPECT_SOME(cgroups::kill(hierarchy, cgroup, SIGKILL));
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));
        }
      }
    }

    CgroupsTest::TearDown();
  }

  const string subsystems; // Subsystems required to run tests.
  string baseHierarchy; // Path to the hierarchy being used.
};


class CgroupsAnyHierarchyWithCpuMemoryTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyWithCpuMemoryTest()
    : CgroupsAnyHierarchyTest("cpu,memory") {}
};


class CgroupsAnyHierarchyWithFreezerTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyWithFreezerTest()
    : CgroupsAnyHierarchyTest("freezer") {}
};


class CgroupsAnyHierarchyDevicesTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyDevicesTest()
    : CgroupsAnyHierarchyTest("devices") {}
};


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Enabled)
{
  EXPECT_SOME_TRUE(cgroups::enabled(""));
  EXPECT_SOME_TRUE(cgroups::enabled(","));
  EXPECT_SOME_TRUE(cgroups::enabled("cpu"));
  EXPECT_SOME_TRUE(cgroups::enabled(",cpu"));
  EXPECT_SOME_TRUE(cgroups::enabled("cpu,memory"));
  EXPECT_SOME_TRUE(cgroups::enabled("cpu,memory,"));
  EXPECT_ERROR(cgroups::enabled("invalid"));
  EXPECT_ERROR(cgroups::enabled("cpu,invalid"));
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_Busy)
{
  EXPECT_SOME_FALSE(cgroups::busy(""));
  EXPECT_SOME_FALSE(cgroups::busy(","));
  EXPECT_SOME_TRUE(cgroups::busy("cpu"));
  EXPECT_SOME_TRUE(cgroups::busy(",cpu"));
  EXPECT_SOME_TRUE(cgroups::busy("cpu,memory"));
  EXPECT_SOME_TRUE(cgroups::busy("cpu,memory,"));
  EXPECT_ERROR(cgroups::busy("invalid"));
  EXPECT_ERROR(cgroups::busy("cpu,invalid"));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Subsystems)
{
  Try<set<string>> names = cgroups::subsystems();
  ASSERT_SOME(names);

  Option<string> cpu;
  Option<string> memory;
  foreach (const string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }

  EXPECT_SOME(cpu);
  EXPECT_SOME(memory);
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_SubsystemsHierarchy)
{
  string cpuHierarchy = path::join(baseHierarchy, "cpu");

  Try<set<string>> names = cgroups::subsystems(cpuHierarchy);
  ASSERT_SOME(names);

  Option<string> cpu;
  Option<string> memory;
  foreach (const string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }

  EXPECT_SOME(cpu);
  EXPECT_NONE(memory);

  string memoryHierarchy = path::join(baseHierarchy, "memory");
  names = cgroups::subsystems(memoryHierarchy);
  ASSERT_SOME(names);

  cpu = None();
  memory = None();
  foreach (const string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }
  EXPECT_NONE(cpu);
  EXPECT_SOME(memory);
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_FindCgroupSubsystems)
{
  pid_t pid = ::getpid();
  Result<string> cpuHierarchy = cgroups::cpu::cgroup(pid);
  EXPECT_FALSE(cpuHierarchy.isError());
  EXPECT_SOME(cpuHierarchy);

  Result<string> memHierarchy = cgroups::memory::cgroup(pid);
  EXPECT_FALSE(memHierarchy.isError());
  EXPECT_SOME(memHierarchy);
}


TEST_F(CgroupsNoHierarchyTest, ROOT_CGROUPS_NOHIERARCHY_MountUnmountHierarchy)
{
  EXPECT_ERROR(cgroups::mount("/tmp", "cpu"));
  EXPECT_ERROR(cgroups::mount(TEST_CGROUPS_HIERARCHY, "invalid"));

  // Try to mount a valid hierarchy, retrying as necessary since the
  // previous unmount might not have taken effect yet due to a bug in
  // Ubuntu 12.04.
  ASSERT_SOME(cgroups::mount(TEST_CGROUPS_HIERARCHY, "cpu,memory", 10));
  EXPECT_ERROR(cgroups::mount(TEST_CGROUPS_HIERARCHY, "cpuset"));
  EXPECT_ERROR(cgroups::unmount("/tmp"));
  ASSERT_SOME(cgroups::unmount(TEST_CGROUPS_HIERARCHY));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Mounted)
{
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp-nonexist"));
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp"));
  EXPECT_SOME_FALSE(cgroups::mounted(baseHierarchy + "/not_expected"));
  EXPECT_SOME_TRUE(cgroups::mounted(baseHierarchy + "/cpu"));
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_MountedSubsystems)
{
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp-nonexist", "cpu"));
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp", "cpu,memory"));
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp", "cpu"));
  EXPECT_SOME_FALSE(cgroups::mounted("/tmp", "invalid"));
  EXPECT_SOME_TRUE(cgroups::mounted(path::join(baseHierarchy, "cpu"), "cpu"));
  EXPECT_SOME_TRUE(cgroups::mounted(
        path::join(baseHierarchy, "memory"), "memory"));
  EXPECT_SOME_FALSE(cgroups::mounted(baseHierarchy, "invalid"));
  EXPECT_SOME_FALSE(cgroups::mounted(baseHierarchy + "/not_expected", "cpu"));
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_CreateRemove)
{
  EXPECT_ERROR(cgroups::create("/tmp", "test"));
  EXPECT_ERROR(cgroups::create(baseHierarchy, "mesos_test_missing/1"));
  ASSERT_SOME(cgroups::create(
        path::join(baseHierarchy, "cpu"), "mesos_test_missing"));
  EXPECT_ERROR(os::rmdir(path::join(baseHierarchy, "invalid"), false));
  ASSERT_SOME(os::rmdir(path::join(
        path::join(baseHierarchy, "cpu"), "mesos_test_missing"), false));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Get)
{
  string hierarchy = path::join(baseHierarchy, "cpu");

  ASSERT_SOME(cgroups::create(hierarchy, "mesos_test1"));
  ASSERT_SOME(cgroups::create(hierarchy, "mesos_test2"));

  Try<vector<string>> cgroups = cgroups::get(hierarchy);
  ASSERT_SOME(cgroups);

  EXPECT_NE(cgroups->end(),
            find(cgroups->begin(), cgroups->end(), "mesos_test2"));
  EXPECT_NE(cgroups->end(),
            find(cgroups->begin(), cgroups->end(), "mesos_test1"));

  ASSERT_SOME(os::rmdir(path::join(hierarchy, "mesos_test1"), false));
  ASSERT_SOME(os::rmdir(path::join(hierarchy, "mesos_test2"), false));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_NestedCgroups)
{
  string hierarchy = path::join(baseHierarchy, "cpu");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  string cgroup1 = path::join(TEST_CGROUPS_ROOT, "1");
  string cgroup2 = path::join(TEST_CGROUPS_ROOT, "2");

  ASSERT_SOME(cgroups::create(hierarchy, cgroup1))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  ASSERT_SOME(cgroups::create(hierarchy, cgroup2));

  Try<vector<string>> cgroups =
    cgroups::get(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_SOME(cgroups);

  ASSERT_EQ(2u, cgroups->size());

  EXPECT_NE(cgroups->end(),
            find(cgroups->begin(), cgroups->end(), cgroup2));
  EXPECT_NE(cgroups->end(),
            find(cgroups->begin(), cgroups->end(), cgroup1));

  ASSERT_SOME(os::rmdir(path::join(hierarchy, cgroup1), false));
  ASSERT_SOME(os::rmdir(path::join(hierarchy, cgroup2), false));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Tasks)
{
  pid_t pid = ::getpid();

  Result<string> cgroup = cgroups::cpu::cgroup(pid);
  ASSERT_SOME(cgroup);

  string hierarchy = path::join(baseHierarchy, "cpu");

  Try<set<pid_t>> pids = cgroups::processes(hierarchy, cgroup.get());
  ASSERT_SOME(pids);

  EXPECT_NE(0u, pids->count(pid));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Read)
{
  string hierarchy = path::join(baseHierarchy, "cpu");

  EXPECT_ERROR(cgroups::read(hierarchy, TEST_CGROUPS_ROOT, "invalid42"));

  pid_t pid = ::getpid();

  Result<string> cgroup = cgroups::cpu::cgroup(pid);
  ASSERT_SOME(cgroup);

  Try<string> read = cgroups::read(hierarchy, cgroup.get(), "tasks");
  ASSERT_SOME(read);

  EXPECT_TRUE(strings::contains(read.get(), stringify(pid)));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Write)
{
  string hierarchy = path::join(baseHierarchy, "cpu");
  EXPECT_ERROR(
      cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "invalid", "invalid"));

  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  EXPECT_ERROR(
      cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "cpu.shares", "invalid"));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process, wait for kill signal.
    while (true) { sleep(1); }

    SAFE_EXIT(
        EXIT_FAILURE, "Error, child should be killed before reaching here");
  }

  // In parent process.
  ASSERT_SOME(
      cgroups::write(hierarchy,
                     TEST_CGROUPS_ROOT,
                     "cgroup.procs",
                     stringify(pid)));

  Try<set<pid_t>> pids = cgroups::processes(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_SOME(pids);

  EXPECT_NE(0u, pids->count(pid));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(pid));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_CFS_Big_Quota)
{
  string hierarchy = path::join(baseHierarchy, "cpu");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  Duration quota = Seconds(100); // Big quota.
  ASSERT_SOME(cgroups::cpu::cfs_quota_us(hierarchy, TEST_CGROUPS_ROOT, quota));

  // Ensure we can read back the correct quota.
  ASSERT_SOME_EQ(
      quota,
      cgroups::cpu::cfs_quota_us(hierarchy, TEST_CGROUPS_ROOT));
}


class CgroupsAnyHierarchyWithCpuAcctMemoryTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyWithCpuAcctMemoryTest()
    : CgroupsAnyHierarchyTest("cpuacct,memory") {}
};


TEST_F(CgroupsAnyHierarchyWithCpuAcctMemoryTest, ROOT_CGROUPS_Stat)
{
  EXPECT_ERROR(cgroups::stat(baseHierarchy, TEST_CGROUPS_ROOT, "invalid"));

  Try<hashmap<string, uint64_t>> result =
    cgroups::stat(
        path::join(baseHierarchy, "cpuacct"), "/", "cpuacct.stat");
  ASSERT_SOME(result);
  EXPECT_TRUE(result->contains("user"));
  EXPECT_TRUE(result->contains("system"));
  EXPECT_GT(result->get("user").get(), 0llu);
  EXPECT_GT(result->get("system").get(), 0llu);

  result = cgroups::stat(
      path::join(baseHierarchy, "memory"), "/", "memory.stat");
  ASSERT_SOME(result);
  EXPECT_TRUE(result->contains("rss"));
  EXPECT_GT(result->get("rss").get(), 0llu);
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_Listen)
{
  string hierarchy = path::join(baseHierarchy, "memory");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  ASSERT_SOME(
      cgroups::memory::oom::killer::enabled(hierarchy, TEST_CGROUPS_ROOT))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  Try<os::Memory> memory = os::memory();
  ASSERT_SOME(memory);

  // TODO(vinod): Instead of asserting here dynamically disable
  // the test if swap is enabled on the host.
  ASSERT_EQ(memory->totalSwap, Bytes(0))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you have swap\n"
    << "enabled, but feel free to disable this test.\n"
    << "-------------------------------------------------------------";

  const Bytes limit = Megabytes(64);

  ASSERT_SOME(cgroups::memory::limit_in_bytes(
      hierarchy, TEST_CGROUPS_ROOT, limit));

  // Listen on oom events for test cgroup.
  Future<Nothing> future =
    cgroups::memory::oom::listen(hierarchy, TEST_CGROUPS_ROOT);

  ASSERT_FALSE(future.isFailed());

  // Test the cancellation.
  future.discard();

  // Test the normal operation below.
  future = cgroups::memory::oom::listen(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_FALSE(future.isFailed());

  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  EXPECT_SOME(cgroups::assign(
      hierarchy, TEST_CGROUPS_ROOT, helper.pid().get()));

  // Request more RSS memory in the subprocess than the limit.
  // NOTE: We enable the kernel oom killer in this test. If it were
  // disabled, the subprocess might hang and the following call won't
  // return. By enabling the oom killer, we let the subprocess get
  // killed and expect that an error is returned.
  EXPECT_ERROR(helper.increaseRSS(limit * 2));

  AWAIT_READY(future);
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_Freeze)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process.
    ::close(pipes[0]);

    // Put self into the test cgroup.
    Try<Nothing> assign =
      cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid());

    if (assign.isError()) {
      std::cerr << "Failed to assign cgroup: " << assign.error() << std::endl;
      abort();
    }

    // Notify the parent.
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      perror("Failed to notify the parent");
      abort();
    }
    ::close(pipes[1]);

    // Infinite loop here.
    while (true);

    // Should not reach here.
    std::cerr << "Reach an unreachable statement!" << std::endl;
    abort();
  }

  // In parent process.
  ::close(pipes[1]);

  // Wait until child has assigned the cgroup.
  ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
  ::close(pipes[0]);

  // Freeze the test cgroup.
  AWAIT_EXPECT_READY(cgroups::freezer::freeze(hierarchy, TEST_CGROUPS_ROOT));

  // Thaw the test cgroup.
  AWAIT_EXPECT_READY(cgroups::freezer::thaw(hierarchy, TEST_CGROUPS_ROOT));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(pid));
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_FreezeNonFreezer)
{
  string hierarchy = path::join(baseHierarchy, "cpu");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  AWAIT_EXPECT_FAILED(cgroups::freezer::freeze(hierarchy, TEST_CGROUPS_ROOT));
  AWAIT_EXPECT_FAILED(cgroups::freezer::thaw(hierarchy, TEST_CGROUPS_ROOT));

  // The cgroup is empty so we should still be able to destroy it.
  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_Kill)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    ::close(pipes[1]);

    // Wait until all children have assigned the cgroup.
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    Try<Nothing> kill = cgroups::kill(hierarchy, TEST_CGROUPS_ROOT, SIGKILL);
    EXPECT_SOME(kill);

    AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(pid));
  } else {
    // In child process.

    // We create 4 child processes here using two forks to test the case in
    // which there are multiple active processes in the given cgroup.
    ::fork();
    ::fork();

    // Put self into the test cgroup.
    Try<Nothing> assign =
      cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid());

    if (assign.isError()) {
      std::cerr << "Failed to assign cgroup: " << assign.error() << std::endl;
      abort();
    }

    // Notify the parent.
    ::close(pipes[0]); // TODO(benh): Close after first fork?
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      perror("Failed to notify the parent");
      abort();
    }
    ::close(pipes[1]);

    // Wait kill signal from parent.
    while (true);

    // Should not reach here.
    std::cerr << "Reach an unreachable statement!" << std::endl;
    abort();
  }
}


// TODO(benh): Write a version of this test with nested cgroups.
TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_Destroy)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    ::close(pipes[1]);

    // Wait until all children have assigned the cgroup.
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));

    // cgroups::destroy will reap all processes in the cgroup so we should
    // *not* be able to reap it now.
    int status;
    EXPECT_EQ(-1, ::waitpid(pid, &status, 0));
    EXPECT_EQ(ECHILD, errno);
  } else {
    // In child process.

    // We create 4 child processes here using two forks to test the case in
    // which there are multiple active processes in the given cgroup.
    ::fork();
    ::fork();

    // Put self into the test cgroup.
    Try<Nothing> assign =
      cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid());

    if (assign.isError()) {
      std::cerr << "Failed to assign cgroup: " << assign.error() << std::endl;
      abort();
    }

    // Notify the parent.
    ::close(pipes[0]); // TODO(benh): Close after first fork?
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      perror("Failed to notify the parent");
      abort();
    }
    ::close(pipes[1]);

    // Wait kill signal from parent.
    while (true) {}

    // Should not reach here.
    std::cerr << "Reach an unreachable statement!" << std::endl;
    abort();
  }
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_AssignThreads)
{
  const size_t numThreads = 5;

  std::thread* runningThreads[numThreads];

  Latch* latch = new Latch();

  // Create additional threads.
  for (size_t i = 0; i < numThreads; i++) {
    runningThreads[i] = new std::thread([=]() {
      // Wait until the main thread tells us to exit.
      latch->await();
    });
  }

  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  // Check the test cgroup is initially empty.
  Try<set<pid_t>> cgroupThreads =
    cgroups::threads(hierarchy, TEST_CGROUPS_ROOT);
  EXPECT_SOME(cgroupThreads);
  EXPECT_TRUE(cgroupThreads->empty());

  // Assign ourselves to the test cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid()));

  // Get our threads (may be more than the numThreads we created if
  // other threads are running).
  Try<set<pid_t>> threads = proc::threads(::getpid());
  ASSERT_SOME(threads);

  // Check the test cgroup now only contains all child threads.
  cgroupThreads = cgroups::threads(hierarchy, TEST_CGROUPS_ROOT);
  EXPECT_SOME(cgroupThreads);
  EXPECT_SOME_EQ(threads.get(), cgroupThreads);

  // Terminate the additional threads.
  latch->trigger();

  for (size_t i = 0; i < numThreads; i++) {
    runningThreads[i]->join();
    delete runningThreads[i];
  }

  delete latch;

  // Move ourselves to the root cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, "", ::getpid()));

  // Destroy the cgroup.
  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_DestroyStoppedProcess)
{
  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process.
    while (true) { sleep(1); }

    ABORT("Child should not reach this statement");
  }

  // In parent process.

  // Put child into the freezer cgroup.
  Try<Nothing> assign = cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, pid);

  // Stop the child process.
  EXPECT_EQ(0, kill(pid, SIGSTOP));

  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));

  // cgroups::destroy will reap all processes in the cgroup so we should
  // *not* be able to reap it now.
  int status;
  EXPECT_EQ(-1, ::waitpid(pid, &status, 0));
  EXPECT_EQ(ECHILD, errno);
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_DestroyTracedProcess)
{
  string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process.
    while (true) { sleep(1); }

    ABORT("Child should not reach this statement");
  }

  // In parent process.
  Try<Nothing> assign = cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, pid);
  ASSERT_SOME(assign);

  // Attach to the child process.
  ASSERT_EQ(0, ptrace(PT_ATTACH, pid, nullptr, nullptr));

  // Wait until the process is in traced state ('t' or 'T').
  Duration elapsed = Duration::zero();
  while (true) {
    Result<proc::ProcessStatus> process = proc::status(pid);
    ASSERT_SOME(process);

    if (process->state == 'T' || process->state == 't') {
      break;
    }

    if (elapsed > Seconds(1)) {
      FAIL() << "Failed to wait for process to be traced";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }

  // Now destroy the cgroup.
  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));

  // cgroups::destroy will reap all processes in the cgroup so we should
  // *not* be able to reap it now.
  int status;
  EXPECT_EQ(-1, ::waitpid(pid, &status, 0));
  EXPECT_EQ(ECHILD, errno);
}


class CgroupsAnyHierarchyWithPerfEventTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyWithPerfEventTest()
    : CgroupsAnyHierarchyTest("perf_event") {}
};


TEST_F(CgroupsAnyHierarchyWithPerfEventTest, ROOT_CGROUPS_PERF_PerfTest)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  string hierarchy = path::join(baseHierarchy, "perf_event");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process.
    ::close(pipes[1]);

    // Wait until parent has assigned us to the cgroup.
    ssize_t len;
    while ((len = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);
    ASSERT_EQ((ssize_t) sizeof(dummy), len);
    ::close(pipes[0]);

    while (true) {
      // Don't sleep so 'perf' can actually sample something.
    }

    ABORT("Child should not reach here");
  }

  // In parent.
  ::close(pipes[0]);

  // Put child into the test cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, pid));

  ssize_t len;
  while ((len = ::write(pipes[1], &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);
  ASSERT_EQ((ssize_t) sizeof(dummy), len);
  ::close(pipes[1]);

  set<string> events;
  // Hardware event.
  events.insert("cycles");
  // Software event.
  events.insert("task-clock");

  // NOTE: Wait at least 2 seconds as we've seen some variance in how
  // well 'perf' does across Linux distributions (e.g., Ubuntu 14.04)
  // and we want to make sure that we collect some non-zero values.
  Future<hashmap<string, mesos::PerfStatistics>> statistics =
    perf::sample(events, {TEST_CGROUPS_ROOT}, Seconds(2));

  AWAIT_READY(statistics);

  ASSERT_TRUE(statistics->contains(TEST_CGROUPS_ROOT));
  ASSERT_TRUE(statistics->at(TEST_CGROUPS_ROOT).has_cycles());

  // TODO(benh): Some Linux distributions (Ubuntu 14.04) fail to
  // properly sample 'cycles' with 'perf', so we don't explicitly
  // check the value here. See MESOS-3082.
  // EXPECT_LT(0u, statistics->cycles());

  ASSERT_TRUE(statistics->at(TEST_CGROUPS_ROOT).has_task_clock());
  EXPECT_LT(0.0, statistics->at(TEST_CGROUPS_ROOT).task_clock());

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(pid));

  // Destroy the cgroup.
  Future<Nothing> destroy = cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT);
  AWAIT_READY(destroy);
}


class CgroupsAnyHierarchyMemoryPressureTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyMemoryPressureTest()
    : CgroupsAnyHierarchyTest("memory"),
      cgroup(TEST_CGROUPS_ROOT) {}

protected:
  void SetUp() override
  {
    CgroupsAnyHierarchyTest::SetUp();

    hierarchy = path::join(baseHierarchy, "memory");

    ASSERT_SOME(cgroups::create(hierarchy, cgroup));
  }

  void listen()
  {
    const vector<Level> levels = {
      Level::LOW,
      Level::MEDIUM,
      Level::CRITICAL
    };

    foreach (Level level, levels) {
      Try<Owned<Counter>> counter = Counter::create(hierarchy, cgroup, level);
      EXPECT_SOME(counter);

      counters[level] = counter.get();
    }
  }

  string hierarchy;
  const string cgroup;

  hashmap<Level, Owned<Counter>> counters;
};


// TODO(alexr): Enable after MESOS-3160 is resolved.
TEST_F(CgroupsAnyHierarchyMemoryPressureTest, DISABLED_ROOT_IncreaseRSS)
{
  Try<os::Memory> memory = os::memory();
  ASSERT_SOME(memory);

  // TODO(vinod): Instead of asserting here dynamically disable
  // the test if swap is enabled on the host.
  ASSERT_EQ(memory->totalSwap, Bytes(0))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you have swap\n"
    << "enabled, but feel free to disable this test.\n"
    << "-------------------------------------------------------------";

  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  const Bytes limit = Megabytes(16);

  // Move the memory test helper into a cgroup and set the limit.
  EXPECT_SOME(cgroups::memory::limit_in_bytes(hierarchy, cgroup, limit));
  EXPECT_SOME(cgroups::assign(hierarchy, cgroup, helper.pid().get()));

  listen();

  // Used to save the counter readings from last iteration.
  uint64_t previousLow = 0;
  uint64_t previousMedium = 0;
  uint64_t previousCritical = 0;

  // Used to save the counter readings from this iteration.
  uint64_t low;
  uint64_t medium;
  uint64_t critical;

  // Use a guard to error out if it's been too long.
  // TODO(chzhcn): Use a better way to set testing time limit.
  uint64_t iterationLimit = (limit.bytes() / os::pagesize()) * 10;
  uint64_t i = 0;
  bool stable = true;
  while (i < iterationLimit) {
    if (stable) {
      EXPECT_SOME(helper.increaseRSS(os::pagesize()));
    }

    Future<uint64_t> _low = counters[Level::LOW]->value();
    Future<uint64_t> _medium = counters[Level::MEDIUM]->value();
    Future<uint64_t> _critical = counters[Level::CRITICAL]->value();

    AWAIT_READY(_low);
    AWAIT_READY(_medium);
    AWAIT_READY(_critical);

    low = _low.get();
    medium = _medium.get();
    critical = _critical.get();

    // We need to know the readings are the same as last time to be
    // sure they are stable, because the reading is not atomic. For
    // example, the medium could turn positive after we read low to be
    // 0, but this should be fixed by the next read immediately.
    if ((low == previousLow &&
         medium == previousMedium &&
         critical == previousCritical)) {
      if (low != 0) {
        EXPECT_LE(medium, low);
        EXPECT_LE(critical, medium);

        // When child's RSS is full, it will be OOM-kill'ed if we
        // don't stop it right away.
        break;
      } else {
        EXPECT_EQ(0u, medium);
        EXPECT_EQ(0u, critical);
      }

      // Counters are stable. Increment iteration count.
      ++i;
      stable = true;
    } else {
      // Counters appear to be unstable. Set the flag to avoid
      // increasing the loop counter and calling increaseRSS()
      // because at this point it is likely to return a failure.
      // The counters should stabilize once we read them again at the
      // next iteratrion (see comment above), so this should never
      // cause an infinite loop.
      stable = false;
    }

    previousLow = low;
    previousMedium = medium;
    previousCritical = critical;
  }
}


TEST_F(CgroupsAnyHierarchyMemoryPressureTest, ROOT_IncreasePageCache)
{
  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  const Bytes limit = Megabytes(16);

  // Move the memory test helper into a cgroup and set the limit.
  EXPECT_SOME(cgroups::memory::limit_in_bytes(hierarchy, cgroup, limit));
  EXPECT_SOME(cgroups::assign(hierarchy, cgroup, helper.pid().get()));

  listen();

  // Used to save the counter readings from last iteration.
  uint64_t previousLow = 0;
  uint64_t previousMedium = 0;
  uint64_t previousCritical = 0;

  // Used to save the counter readings from this iteration.
  uint64_t low;
  uint64_t medium;
  uint64_t critical;

  // Use a guard to error out if it's been too long.
  // TODO(chzhcn): Use a better way to set testing time limit.
  uint64_t iterationLimit = limit.bytes() / Megabytes(1).bytes() * 2;

  for (uint64_t i = 0; i < iterationLimit; i++) {
    EXPECT_SOME(helper.increasePageCache(Megabytes(1)));

    Future<uint64_t> _low = counters[Level::LOW]->value();
    Future<uint64_t> _medium = counters[Level::MEDIUM]->value();
    Future<uint64_t> _critical = counters[Level::CRITICAL]->value();

    AWAIT_READY(_low);
    AWAIT_READY(_medium);
    AWAIT_READY(_critical);

    low = _low.get();
    medium = _medium.get();
    critical = _critical.get();

    // We need to know the readings are the same as last time to be
    // sure they are stable, because the reading is not atomic. For
    // example, the medium could turn positive after we read low to be
    // 0, but this should be fixed by the next read immediately.
    if ((low == previousLow &&
         medium == previousMedium &&
         critical == previousCritical)) {
      if (low != 0) {
        EXPECT_LE(medium, low);
        EXPECT_LE(critical, medium);

        // Different from the RSS test, since the child is only
        // consuming at a slow rate the page cache, which is evictable
        // and reclaimable, we could therefore be in this state
        // forever. Our guard will let us out shortly.
      } else {
        EXPECT_EQ(0u, medium);
        EXPECT_EQ(0u, critical);
      }
    }

    previousLow = low;
    previousMedium = medium;
    previousCritical = critical;
  }

  EXPECT_LT(0u, low);
}


// Tests the cpuacct::stat API. This test just tests for ANY value returned by
// the API.
TEST_F(CgroupsAnyHierarchyWithCpuAcctMemoryTest, ROOT_CGROUPS_CpuAcctsStats)
{
  const string hierarchy = path::join(baseHierarchy, "cpuacct");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  ASSERT_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid()));

  ASSERT_SOME(cgroups::cpuacct::stat(hierarchy, TEST_CGROUPS_ROOT));

  // Move ourselves to the root cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, "", ::getpid()));

  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
}


// TODO(klueska): Ideally we would call this test CgroupsDevicesTest,
// but currently we filter tests on the keyword 'Cgroup' and only run
// them if we have root access. However, this test doesn't require
// root access since it's just testing the parsing aspects of the
// cgroups devices whitelist. If we ever modify this filter to be less
// restrictive, we should rename this test accordingly.
TEST(DevicesTest, Parse)
{
  EXPECT_ERROR(cgroups::devices::Entry::parse(""));
  EXPECT_ERROR(cgroups::devices::Entry::parse("x"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b x"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b *:*"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b *:* x"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b *:* rwmx"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b *:* rwm x"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b x:x rwm"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b *:x rwm"));
  EXPECT_ERROR(cgroups::devices::Entry::parse("b x:* rwm"));

  Try<cgroups::devices::Entry> entry =
    cgroups::devices::Entry::parse("a");
  EXPECT_EQ("a *:* rwm", stringify(entry.get()));

  // Note that if the first character is "a", the rest
  // of the input is ignored, even if it is invalid!
  entry = cgroups::devices::Entry::parse("a x x");
  EXPECT_EQ("a *:* rwm", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b *:* rwm");
  EXPECT_EQ("b *:* rwm", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b *:* r");
  EXPECT_EQ("b *:* r", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b *:* w");
  EXPECT_EQ("b *:* w", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b *:* m");
  EXPECT_EQ("b *:* m", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b 1:* rwm");
  EXPECT_EQ("b 1:* rwm", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b *:3 rwm");
  EXPECT_EQ("b *:3 rwm", stringify(entry.get()));

  entry = cgroups::devices::Entry::parse("b 1:3 rwm");
  EXPECT_EQ("b 1:3 rwm", stringify(entry.get()));
}


TEST_F(CgroupsAnyHierarchyDevicesTest, ROOT_CGROUPS_Devices)
{
  // Create a cgroup in the devices hierarchy.
  string hierarchy = path::join(baseHierarchy, "devices");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  // Assign ourselves to the cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid()));

  // When a devices cgroup is first created, its whitelist inherits
  // all devices from its parent's whitelist (i.e., "a *:* rwm" by
  // default). In theory, we should be able to add and remove devices
  // from the whitelist by writing to the respective `devices.allow`
  // and `devices.deny` files associated with the cgroup. However, the
  // semantics of the whitelist are such that writing to the deny file
  // will only remove entries in the whitelist that are explicitly
  // listed in there (i.e., denying "b 1:3 rwm" when the whitelist
  // only contains "a *:* rwm" will not modify the whitelist because
  // "b 1:3 rwm" is not explicitly listed).  Although the whitelist
  // doesn't change, access to the device is still denied as expected
  // (there is just no way of querying the system to detect it).
  // Because of this, we first deny access to all devices and
  // selectively add some back in so we can control the entries in the
  // whitelist explicitly.
  Try<cgroups::devices::Entry> entry =
    cgroups::devices::Entry::parse("a *:* rwm");

  ASSERT_SOME(entry);

  Try<Nothing> deny = cgroups::devices::deny(
      hierarchy,
      TEST_CGROUPS_ROOT,
      entry.get());

  EXPECT_SOME(deny);

  // Verify that there are no entries in the whitelist.
  Try<vector<cgroups::devices::Entry>> whitelist =
      cgroups::devices::list(hierarchy, TEST_CGROUPS_ROOT);

  ASSERT_SOME(whitelist);

  EXPECT_TRUE(whitelist->empty());

  // Verify that we can't open /dev/null.
  Try<int_fd> fd = os::open(os::DEV_NULL, O_RDWR);
  EXPECT_ERROR(fd);

  // Add /dev/null to the list of allowed devices.
  entry = cgroups::devices::Entry::parse("c 1:3 rwm");

  ASSERT_SOME(entry);

  Try<Nothing> allow = cgroups::devices::allow(
      hierarchy,
      TEST_CGROUPS_ROOT,
      entry.get());

  EXPECT_SOME(allow);

  // Verify that /dev/null is in the whitelist.
  whitelist = cgroups::devices::list(hierarchy, TEST_CGROUPS_ROOT);

  ASSERT_SOME(whitelist);

  ASSERT_EQ(1u, whitelist->size());

  EXPECT_EQ(entry.get(), whitelist.get()[0]);

  // Verify that we can now open and write to /dev/null.
  fd = os::open(os::DEV_NULL, O_WRONLY);
  ASSERT_SOME(fd);

  Try<Nothing> write = os::write(fd.get(), "nonsense");
  EXPECT_SOME(write);

  // Move ourselves to the root cgroup.
  ASSERT_SOME(cgroups::assign(hierarchy, "", ::getpid()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
