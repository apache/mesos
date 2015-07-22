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

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gmock/gmock.h>

#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"
#include "linux/perf.hpp"

#include "tests/memory_test_helper.hpp"
#include "tests/mesos.hpp" // For TEST_CGROUPS_(HIERARCHY|ROOT).
#include "tests/utils.hpp"

using namespace process;

using cgroups::memory::pressure::Level;
using cgroups::memory::pressure::Counter;

using std::set;

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

    Try<std::set<std::string> > hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    ASSERT_TRUE(hierarchies.get().empty())
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
  CgroupsAnyHierarchyTest(const std::string& _subsystems = "cpu")
    : subsystems(_subsystems) {}

protected:
  virtual void SetUp()
  {
    CgroupsTest::SetUp();

    foreach (const std::string& subsystem, strings::tokenize(subsystems, ",")) {
      // Establish the base hierarchy if this is the first subsystem checked.
      if (baseHierarchy.empty()) {
        Result<std::string> hierarchy = cgroups::hierarchy(subsystem);
        ASSERT_FALSE(hierarchy.isError());

        if (hierarchy.isNone()) {
          baseHierarchy = TEST_CGROUPS_HIERARCHY;
        } else {
          // Strip the subsystem to get the base hierarchy.
          Try<std::string> baseDirname = Path(hierarchy.get()).dirname();
          ASSERT_SOME(baseDirname);
          baseHierarchy = baseDirname.get();
        }
      }

      // Mount the subsystem if necessary.
      std::string hierarchy = path::join(baseHierarchy, subsystem);
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

      Try<std::vector<std::string> > cgroups = cgroups::get(hierarchy);
      CHECK_SOME(cgroups);

      foreach (const std::string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT.
        if (cgroup == TEST_CGROUPS_ROOT) {
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));
        }
      }
    }
  }

  virtual void TearDown()
  {
    // Remove all *our* cgroups.
    foreach (const std::string& subsystem, strings::tokenize(subsystems, ",")) {
      std::string hierarchy = path::join(baseHierarchy, subsystem);

      Try<std::vector<std::string> > cgroups = cgroups::get(hierarchy);
      CHECK_SOME(cgroups);

      foreach (const std::string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT.
        if (cgroup == TEST_CGROUPS_ROOT) {
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));
        }
      }
    }

    CgroupsTest::TearDown();
  }

  const std::string subsystems; // Subsystems required to run tests.
  std::string baseHierarchy; // Path to the hierarchy being used.
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
  Try<std::set<std::string> > names = cgroups::subsystems();
  ASSERT_SOME(names);

  Option<std::string> cpu;
  Option<std::string> memory;
  foreach (const std::string& name, names.get()) {
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
  std::string cpuHierarchy = path::join(baseHierarchy, "cpu");

  Try<std::set<std::string> > names = cgroups::subsystems(cpuHierarchy);
  ASSERT_SOME(names);

  Option<std::string> cpu;
  Option<std::string> memory;
  foreach (const std::string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }

  EXPECT_SOME(cpu);
  EXPECT_NONE(memory);

  std::string memoryHierarchy = path::join(baseHierarchy, "memory");
  names = cgroups::subsystems(memoryHierarchy);
  ASSERT_SOME(names);

  cpu = None();
  memory = None();
  foreach (const std::string& name, names.get()) {
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
  Result<std::string> cpuHierarchy = cgroups::cpu::cgroup(pid);
  EXPECT_FALSE(cpuHierarchy.isError());
  EXPECT_SOME(cpuHierarchy);

  Result<std::string> memHierarchy = cgroups::memory::cgroup(pid);
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
  EXPECT_ERROR(cgroups::remove(baseHierarchy, "invalid"));
  ASSERT_SOME(cgroups::remove(
        path::join(baseHierarchy, "cpu"), "mesos_test_missing"));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Get)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");

  ASSERT_SOME(cgroups::create(hierarchy, "mesos_test1"));
  ASSERT_SOME(cgroups::create(hierarchy, "mesos_test2"));

  Try<std::vector<std::string>> cgroups = cgroups::get(hierarchy);
  ASSERT_SOME(cgroups);

  EXPECT_NE(cgroups.get().end(),
            find(cgroups.get().begin(), cgroups.get().end(), "mesos_test2"));
  EXPECT_NE(cgroups.get().end(),
            find(cgroups.get().begin(), cgroups.get().end(), "mesos_test1"));

  ASSERT_SOME(cgroups::remove(hierarchy, "mesos_test1"));
  ASSERT_SOME(cgroups::remove(hierarchy, "mesos_test2"));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_NestedCgroups)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  std::string cgroup1 = path::join(TEST_CGROUPS_ROOT, "1");
  std::string cgroup2 = path::join(TEST_CGROUPS_ROOT, "2");

  ASSERT_SOME(cgroups::create(hierarchy, cgroup1))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  ASSERT_SOME(cgroups::create(hierarchy, cgroup2));

  Try<std::vector<std::string>> cgroups =
    cgroups::get(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_SOME(cgroups);

  ASSERT_EQ(2u, cgroups.get().size());

  EXPECT_NE(cgroups.get().end(),
            find(cgroups.get().begin(), cgroups.get().end(), cgroup2));
  EXPECT_NE(cgroups.get().end(),
            find(cgroups.get().begin(), cgroups.get().end(), cgroup1));

  ASSERT_SOME(cgroups::remove(hierarchy, cgroup1));
  ASSERT_SOME(cgroups::remove(hierarchy, cgroup2));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Tasks)
{
  pid_t pid = ::getpid();

  Result<std::string> cgroup = cgroups::cpu::cgroup(pid);
  ASSERT_SOME(cgroup);

  std::string hierarchy = path::join(baseHierarchy, "cpu");

  Try<std::set<pid_t>> pids = cgroups::processes(hierarchy, cgroup.get());
  ASSERT_SOME(pids);

  EXPECT_NE(0u, pids.get().count(pid));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Read)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");

  EXPECT_ERROR(cgroups::read(hierarchy, TEST_CGROUPS_ROOT, "invalid42"));

  pid_t pid = ::getpid();

  Result<std::string> cgroup = cgroups::cpu::cgroup(pid);
  ASSERT_SOME(cgroup);

  Try<std::string> read = cgroups::read(hierarchy, cgroup.get(), "tasks");
  ASSERT_SOME(read);

  EXPECT_TRUE(strings::contains(read.get(), stringify(pid)));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Write)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  EXPECT_ERROR(
      cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "invalid", "invalid"));

  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process, wait for kill signal.
    while (true) { sleep(1); }

    // Should not reach here.
    const char* message = "Error, child should be killed before reaching here";
    while (write(STDERR_FILENO, message, strlen(message)) == -1 &&
           errno == EINTR);

    _exit(1);
  }

  // In parent process.
  ASSERT_SOME(
      cgroups::write(hierarchy,
                     TEST_CGROUPS_ROOT,
                     "cgroup.procs",
                     stringify(pid)));

  Try<std::set<pid_t> > pids = cgroups::processes(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_SOME(pids);

  EXPECT_NE(0u, pids.get().count(pid));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  int status;
  EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Cfs_Big_Quota)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
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

  Try<hashmap<std::string, uint64_t> > result =
    cgroups::stat(
        path::join(baseHierarchy, "cpuacct"), "/", "cpuacct.stat");
  ASSERT_SOME(result);
  EXPECT_TRUE(result.get().contains("user"));
  EXPECT_TRUE(result.get().contains("system"));
  EXPECT_GT(result.get().get("user").get(), 0llu);
  EXPECT_GT(result.get().get("system").get(), 0llu);

  result = cgroups::stat(
      path::join(baseHierarchy, "memory"), "/", "memory.stat");
  ASSERT_SOME(result);
  EXPECT_TRUE(result.get().contains("rss"));
  EXPECT_GT(result.get().get("rss").get(), 0llu);
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_Listen)
{
  std::string hierarchy = path::join(baseHierarchy, "memory");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  ASSERT_SOME(
      cgroups::memory::oom::killer::enabled(hierarchy, TEST_CGROUPS_ROOT))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  const Bytes limit =  Megabytes(64);

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

  std::string hierarchy = path::join(baseHierarchy, "freezer");
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
  int status;
  EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_FreezeNonFreezer)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
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

  std::string hierarchy = path::join(baseHierarchy, "freezer");
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

    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(SIGKILL, WTERMSIG(status));
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

  std::string hierarchy = path::join(baseHierarchy, "freezer");
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


void* threadFunction(void*)
{
  // Newly created threads have PTHREAD_CANCEL_ENABLE and
  // PTHREAD_CANCEL_DEFERRED so they can be cancelled from the main thread.
  while (true) { sleep(1); }

  return NULL;
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_AssignThreads)
{
  size_t numThreads = 5;

  pthread_t pthreads[numThreads];

  // Create additional threads.
  for (size_t i = 0; i < numThreads; i++)
  {
    EXPECT_EQ(0, pthread_create(&pthreads[i], NULL, threadFunction, NULL));
  }

  std::string hierarchy = path::join(baseHierarchy, "freezer");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  // Check the test cgroup is initially empty.
  Try<set<pid_t> > cgroupThreads =
    cgroups::threads(hierarchy, TEST_CGROUPS_ROOT);
  EXPECT_SOME(cgroupThreads);
  EXPECT_EQ(0u, cgroupThreads.get().size());

  // Assign ourselves to the test cgroup.
  CHECK_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid()));

  // Get our threads (may be more than the numThreads we created if
  // other threads are running).
  Try<set<pid_t> > threads = proc::threads(::getpid());
  ASSERT_SOME(threads);

  // Check the test cgroup now only contains all child threads.
  cgroupThreads = cgroups::threads(hierarchy, TEST_CGROUPS_ROOT);
  EXPECT_SOME(cgroupThreads);
  EXPECT_SOME_EQ(threads.get(), cgroupThreads);

  // Terminate the additional threads.
  for (size_t i = 0; i < numThreads; i++)
  {
    EXPECT_EQ(0, pthread_cancel(pthreads[i]));
    EXPECT_EQ(0, pthread_join(pthreads[i], NULL));
  }

  // Move ourselves to the root cgroup.
  CHECK_SOME(cgroups::assign(hierarchy, "", ::getpid()));

  // Destroy the cgroup.
  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
}


TEST_F(CgroupsAnyHierarchyWithFreezerTest, ROOT_CGROUPS_DestroyStoppedProcess)
{
  std::string hierarchy = path::join(baseHierarchy, "freezer");
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
  std::string hierarchy = path::join(baseHierarchy, "freezer");
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
  ASSERT_EQ(0, ptrace(PT_ATTACH, pid, NULL, NULL));

  // Wait until the process is in traced state ('t' or 'T').
  Duration elapsed = Duration::zero();
  while (true) {
    Result<proc::ProcessStatus> process = proc::status(pid);
    ASSERT_SOME(process);

    if (process.get().state == 'T' || process.get().state == 't') {
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


TEST_F(CgroupsAnyHierarchyWithPerfEventTest, ROOT_CGROUPS_Perf)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  std::string hierarchy = path::join(baseHierarchy, "perf_event");
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

  std::set<std::string> events;
  // Hardware event.
  events.insert("cycles");
  // Software event.
  events.insert("task-clock");

  // NOTE: Wait at least 2 seconds as we've seen some variance in how
  // well 'perf' does across Linux distributions (e.g., Ubuntu 14.04)
  // and we want to make sure that we collect some non-zero values.
  Future<mesos::PerfStatistics> statistics =
    perf::sample(events, TEST_CGROUPS_ROOT, Seconds(2));
  AWAIT_READY(statistics);

  ASSERT_TRUE(statistics.get().has_cycles());

  // TODO(benh): Some Linux distributions (Ubuntu 14.04) fail to
  // properly sample 'cycles' with 'perf', so we don't explicitly
  // check the value here. See MESOS-3082.
  // EXPECT_LT(0u, statistics.get().cycles());

  ASSERT_TRUE(statistics.get().has_task_clock());
  EXPECT_LT(0.0, statistics.get().task_clock());

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  int status;
  EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));

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
  virtual void SetUp()
  {
    CgroupsAnyHierarchyTest::SetUp();

    hierarchy = path::join(baseHierarchy, "memory");

    ASSERT_SOME(cgroups::create(hierarchy, cgroup));
  }

  void listen()
  {
    const std::vector<Level> levels = {
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

  std::string hierarchy;
  const std::string cgroup;

  hashmap<Level, Owned<Counter>> counters;
};


TEST_F(CgroupsAnyHierarchyMemoryPressureTest, ROOT_IncreaseUnlockedRSS)
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
  uint64_t iterationLimit = limit.bytes() / getpagesize() * 10;

  for (uint64_t i = 0; i < iterationLimit; i++) {
    EXPECT_SOME(helper.increaseRSS(getpagesize()));

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
  const std::string hierarchy = path::join(baseHierarchy, "cpuacct");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  CHECK_SOME(cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid()));

  ASSERT_SOME(cgroups::cpuacct::stat(hierarchy, TEST_CGROUPS_ROOT));

  // Move ourselves to the root cgroup.
  CHECK_SOME(cgroups::assign(hierarchy, "", ::getpid()));

  AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
