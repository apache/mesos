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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gmock/gmock.h>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"

#include "tests/mesos.hpp" // For TEST_CGROUPS_(HIERARCHY|ROOT).

using namespace mesos::internal::tests;

using namespace process;


class CgroupsTest : public ::testing::Test
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
    foreach (const std::string& subsystem, strings::tokenize(subsystems, ",")) {
      // Establish the base hierarchy if this is the first subsystem checked.
      if (baseHierarchy.empty()) {
        Result<std::string> hierarchy = cgroups::hierarchy(subsystem);
        ASSERT_FALSE(hierarchy.isError());

        if (hierarchy.isNone()) {
          baseHierarchy = TEST_CGROUPS_HIERARCHY;
        } else {
          // Strip the subsystem to get the base hierarchy.
          baseHierarchy = strings::remove(
              hierarchy.get(),
              subsystem,
              strings::SUFFIX);
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


class CgroupsAnyHierarchyWithCpuMemoryFreezerTest
  : public CgroupsAnyHierarchyTest
{
public:
  CgroupsAnyHierarchyWithCpuMemoryFreezerTest()
    : CgroupsAnyHierarchyTest("cpu,memory,freezer") {}
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

  Try<std::vector<std::string> > cgroups = cgroups::get(hierarchy);
  ASSERT_SOME(cgroups);

  EXPECT_EQ(cgroups.get()[0], "mesos_test2");
  EXPECT_EQ(cgroups.get()[1], "mesos_test1");

  ASSERT_SOME(cgroups::remove(hierarchy, "mesos_test1"));
  ASSERT_SOME(cgroups::remove(hierarchy, "mesos_test2"));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_NestedCgroups)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  ASSERT_SOME(cgroups::create(hierarchy, path::join(TEST_CGROUPS_ROOT, "1")))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  ASSERT_SOME(cgroups::create(hierarchy, path::join(TEST_CGROUPS_ROOT, "2")));

  Try<std::vector<std::string> > cgroups =
    cgroups::get(hierarchy, TEST_CGROUPS_ROOT);

  ASSERT_SOME(cgroups);
  ASSERT_EQ(2u, cgroups.get().size());

  EXPECT_EQ(cgroups.get()[0], path::join(TEST_CGROUPS_ROOT, "2"));
  EXPECT_EQ(cgroups.get()[1], path::join(TEST_CGROUPS_ROOT, "1"));

  ASSERT_SOME(cgroups::remove(hierarchy, path::join(TEST_CGROUPS_ROOT, "1")));
  ASSERT_SOME(cgroups::remove(hierarchy, path::join(TEST_CGROUPS_ROOT, "2")));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Tasks)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  Try<std::set<pid_t> > pids = cgroups::processes(hierarchy, "/");
  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().count(1));
  EXPECT_NE(0u, pids.get().count(::getpid()));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Read)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  EXPECT_ERROR(cgroups::read(hierarchy, TEST_CGROUPS_ROOT, "invalid"));

  std::string pid = stringify(::getpid());

  Try<std::string> result = cgroups::read(hierarchy, "/", "tasks");
  ASSERT_SOME(result);
  EXPECT_TRUE(strings::contains(result.get(), pid));
}


TEST_F(CgroupsAnyHierarchyTest, ROOT_CGROUPS_Write)
{
  std::string hierarchy = path::join(baseHierarchy, "cpu");
  EXPECT_ERROR(
      cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "invalid", "invalid"));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));

  if (pid > 0) {
    // In parent process.
    ASSERT_SOME(
        cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "tasks", stringify(pid)));

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
  } else {
    // In child process, wait for kill signal.
    while (true);

    // Should not reach here.
    std::cerr << "Reach an unreachable statement!" << std::endl;
    abort();
  }
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
  EXPECT_GT(result.get()["user"], 0llu);
  EXPECT_GT(result.get()["system"], 0llu);

  result = cgroups::stat(
      path::join(baseHierarchy, "memory"), "/", "memory.stat");
  ASSERT_SOME(result);
  EXPECT_TRUE(result.get().contains("rss"));
  EXPECT_GT(result.get()["rss"], 0llu);
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryTest, ROOT_CGROUPS_Listen)
{
  std::string hierarchy = path::join(baseHierarchy, "memory");
  ASSERT_SOME(cgroups::create(hierarchy, TEST_CGROUPS_ROOT));
  ASSERT_SOME(
      cgroups::exists(hierarchy, TEST_CGROUPS_ROOT, "memory.oom_control"))
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because it appears you do not have\n"
    << "a modern enough version of the Linux kernel. You won't be\n"
    << "able to use the cgroups isolator, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  // Disable oom killer.
  ASSERT_SOME(
      cgroups::write(hierarchy, TEST_CGROUPS_ROOT, "memory.oom_control", "1"));

  // Limit the memory usage of the test cgroup to 64MB.
  size_t limit = 1024 * 1024 * 64;
  ASSERT_SOME(cgroups::write(
                  hierarchy,
                  TEST_CGROUPS_ROOT,
                  "memory.limit_in_bytes",
                  stringify(limit)));

  // Listen on oom events for test cgroup.
  Future<uint64_t> future = cgroups::listen(
      hierarchy, TEST_CGROUPS_ROOT, "memory.oom_control");
  ASSERT_FALSE(future.isFailed());

  // Test the cancellation.
  future.discard();

  // Test the normal operation below.
  future = cgroups::listen(hierarchy, TEST_CGROUPS_ROOT, "memory.oom_control");
  ASSERT_FALSE(future.isFailed());

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    future.await(Seconds(5));

    EXPECT_TRUE(future.isReady());

    // Kill the child process.
    EXPECT_NE(-1, ::kill(pid, SIGKILL));

    // Wait for the child process.
    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(SIGKILL, WTERMSIG(status));
  } else {
    // In child process. We try to trigger an oom here.
    // Put self into the test cgroup.
    Try<Nothing> assign =
      cgroups::assign(hierarchy, TEST_CGROUPS_ROOT, ::getpid());

    if (assign.isError()) {
      std::cerr << "Failed to assign cgroup: " << assign.error() << std::endl;
      abort();
    }

    // Blow up the memory.
    size_t limit = 1024 * 1024 * 512;
    void* buffer = NULL;

    if (posix_memalign(&buffer, getpagesize(), limit) != 0) {
      perror("Failed to allocate page-aligned memory, posix_memalign");
      abort();
    }

    // We use mlock and memset here to make sure that the memory
    // actually gets paged in and thus accounted for.
    if (mlock(buffer, limit) != 0) {
      perror("Failed to lock memory, mlock");
      abort();
    }

    if (memset(buffer, 1, limit) != buffer) {
      perror("Failed to fill memory, memset");
      abort();
    }

    // Should not reach here.
    std::cerr << "OOM does not happen!" << std::endl;
    abort();
  }
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryFreezerTest, ROOT_CGROUPS_Freeze)
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

    // Wait until child has assigned the cgroup.
    ASSERT_LT(0, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    // Freeze the test cgroup.
    Future<bool> freeze = cgroups::freeze(hierarchy, TEST_CGROUPS_ROOT);
    freeze.await(Seconds(5));
    ASSERT_TRUE(freeze.isReady());
    EXPECT_EQ(true, freeze.get());

    // Thaw the test cgroup.
    Future<bool> thaw = cgroups::thaw(hierarchy, TEST_CGROUPS_ROOT);
    thaw.await(Seconds(5));
    ASSERT_TRUE(thaw.isReady());
    EXPECT_EQ(true, thaw.get());

    // Kill the child process.
    ASSERT_NE(-1, ::kill(pid, SIGKILL));

    // Wait for the child process.
    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(SIGKILL, WTERMSIG(status));
  } else {
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
}


TEST_F(CgroupsAnyHierarchyWithCpuMemoryFreezerTest, ROOT_CGROUPS_Kill)
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
TEST_F(CgroupsAnyHierarchyWithCpuMemoryFreezerTest, ROOT_CGROUPS_Destroy)
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

    Future<bool> future = cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT);
    future.await(Seconds(5));
    ASSERT_TRUE(future.isReady());
    EXPECT_TRUE(future.get());

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
    while (true) ;

    // Should not reach here.
    std::cerr << "Reach an unreachable statement!" << std::endl;
    abort();
  }
}
