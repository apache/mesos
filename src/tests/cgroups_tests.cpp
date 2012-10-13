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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gmock/gmock.h>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"

#include "tests/utils.hpp"

using namespace process;


// Define the test fixture for the cgroups tests.
class CgroupsTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    cleanup();
    prepare();
  }

  virtual void TearDown()
  {
    cleanup();
  }

  // Prepare the testing hierarchy and cgroups.
  void prepare()
  {
    // Create a hierarchy for test.
    std::string subsystems = "cpu,memory,freezer";
    ASSERT_SOME(cgroups::createHierarchy(hierarchy, subsystems));

    // Create cgroups for test.
    ASSERT_SOME(cgroups::createCgroup(hierarchy, "prof"));
    ASSERT_SOME(cgroups::createCgroup(hierarchy, "stu"));
    ASSERT_SOME(cgroups::createCgroup(hierarchy, "stu/grad"));
    ASSERT_SOME(cgroups::createCgroup(hierarchy, "stu/under"));
    ASSERT_SOME(cgroups::createCgroup(hierarchy, "stu/under/senior"));
  }

  void cleanup()
  {
    if (cgroups::checkHierarchy(hierarchy).isSome()) {
      // Remove all cgroups.
      Try<std::vector<std::string> > cgroups = cgroups::getCgroups(hierarchy);
      ASSERT_SOME(cgroups);
      foreach (const std::string& cgroup, cgroups.get()) {
        ASSERT_SOME(cgroups::removeCgroup(hierarchy, cgroup));
      }

      // Remove the hierarchy.
      ASSERT_SOME(cgroups::removeHierarchy(hierarchy));
    }

    // Remove the directory if still exists.
    if (os::exists(hierarchy)) {
      os::rmdir(hierarchy);
    }
  }

  // Path to the root hierarchy for tests.
  static const std::string hierarchy;
};


// Define the test fixture for the simple cgroups tests. Simple cgroups tests do
// not prepare testing hierarchy and cgroups.
class CgroupsSimpleTest : public CgroupsTest
{
protected:
  virtual void SetUp()
  {
    cleanup();
  }
};


const std::string CgroupsTest::hierarchy = "/tmp/mesos_cgroups_test_hierarchy";


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_Enabled)
{
  EXPECT_SOME_TRUE(cgroups::enabled("cpu"));
  EXPECT_SOME_TRUE(cgroups::enabled(",cpu"));
  EXPECT_SOME_TRUE(cgroups::enabled("cpu,memory"));
  EXPECT_SOME_TRUE(cgroups::enabled("cpu,memory,"));
  EXPECT_ERROR(cgroups::enabled("invalid"));
  EXPECT_ERROR(cgroups::enabled("cpu,invalid"));
  EXPECT_ERROR(cgroups::enabled(","));
  EXPECT_ERROR(cgroups::enabled(""));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_Busy)
{
  EXPECT_ERROR(cgroups::busy("invalid"));
  EXPECT_ERROR(cgroups::busy("cpu,invalid"));
  EXPECT_ERROR(cgroups::busy(","));
  EXPECT_ERROR(cgroups::busy(""));
  EXPECT_SOME_TRUE(cgroups::busy("cpu"));
  EXPECT_SOME_TRUE(cgroups::busy(",cpu"));
  EXPECT_SOME_TRUE(cgroups::busy("cpu,memory"));
  EXPECT_SOME_TRUE(cgroups::busy("cpu,memory,"));
}


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_Subsystems)
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


TEST_F(CgroupsTest, ROOT_CGROUPS_SubsystemsHierarchy)
{
  Try<std::set<std::string> > names = cgroups::subsystems(hierarchy);
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


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_CreateRemoveHierarchy)
{
  EXPECT_ERROR(cgroups::createHierarchy("/tmp", "cpu"));

  EXPECT_ERROR(cgroups::createHierarchy(hierarchy, "invalid"));

  ASSERT_SOME(cgroups::createHierarchy(hierarchy, "cpu,memory"));

  EXPECT_ERROR(cgroups::createHierarchy(hierarchy, "cpuset"));

  EXPECT_ERROR(cgroups::removeHierarchy("/tmp"));

  ASSERT_SOME(cgroups::removeHierarchy(hierarchy));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_CheckHierarchy)
{
  EXPECT_ERROR(cgroups::checkHierarchy("/tmp-nonexist"));

  EXPECT_ERROR(cgroups::checkHierarchy("/tmp"));

  EXPECT_SOME(cgroups::checkHierarchy(hierarchy));

  EXPECT_SOME(cgroups::checkHierarchy(hierarchy + "/"));

  EXPECT_ERROR(cgroups::checkHierarchy(hierarchy + "/stu"));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_CheckHierarchySubsystems)
{
  EXPECT_ERROR(cgroups::checkHierarchy("/tmp-nonexist", "cpu"));

  EXPECT_ERROR(cgroups::checkHierarchy("/tmp", "cpu,memory"));

  EXPECT_ERROR(cgroups::checkHierarchy("/tmp", "cpu"));

  EXPECT_ERROR(cgroups::checkHierarchy("/tmp", "invalid"));

  EXPECT_SOME(cgroups::checkHierarchy(hierarchy, "cpu,memory"));

  EXPECT_SOME(cgroups::checkHierarchy(hierarchy, "memory"));

  EXPECT_ERROR(cgroups::checkHierarchy(hierarchy, "invalid"));

  EXPECT_ERROR(cgroups::checkHierarchy(hierarchy + "/stu", "cpu"));
}


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_CreateRemoveCgroup)
{
  EXPECT_ERROR(cgroups::createCgroup("/tmp", "test"));

  ASSERT_SOME(cgroups::createHierarchy(hierarchy, "cpu,memory"));

  EXPECT_ERROR(cgroups::createCgroup(hierarchy, "test/1"));

  ASSERT_SOME(cgroups::createCgroup(hierarchy, "test"));

  EXPECT_ERROR(cgroups::removeCgroup(hierarchy, "invalid"));

  ASSERT_SOME(cgroups::removeCgroup(hierarchy, "test"));

  ASSERT_SOME(cgroups::removeHierarchy(hierarchy));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_ReadControl)
{
  EXPECT_ERROR(cgroups::readControl(hierarchy, "/stu", "invalid"));

  std::string pid = stringify(::getpid());

  Try<std::string> result = cgroups::readControl(hierarchy, "/", "tasks");
  ASSERT_SOME(result);
  EXPECT_TRUE(strings::contains(result.get(), pid));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_WriteControl)
{
  EXPECT_ERROR(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "invalid",
                                    "invalid"));

  std::string pid = stringify(::getpid());

  ASSERT_SOME(cgroups::writeControl(hierarchy, "/prof", "tasks", pid));

  Try<std::set<pid_t> > tasks = cgroups::getTasks(hierarchy, "/prof");
  ASSERT_SOME(tasks);

  std::set<pid_t> pids = tasks.get();
  EXPECT_NE(pids.find(::getpid()), pids.end());

  ASSERT_SOME(cgroups::writeControl(hierarchy, "/", "tasks", pid));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_GetCgroups)
{
  Try<std::vector<std::string> > cgroups = cgroups::getCgroups(hierarchy);
  ASSERT_SOME(cgroups);

  EXPECT_EQ(cgroups.get()[0], "/stu/under/senior");
  EXPECT_EQ(cgroups.get()[1], "/stu/under");
  EXPECT_EQ(cgroups.get()[2], "/stu/grad");
  EXPECT_EQ(cgroups.get()[3], "/stu");
  EXPECT_EQ(cgroups.get()[4], "/prof");

  cgroups = cgroups::getCgroups(hierarchy, "/stu");
  ASSERT_SOME(cgroups);

  EXPECT_EQ(cgroups.get()[0], "/stu/under/senior");
  EXPECT_EQ(cgroups.get()[1], "/stu/under");
  EXPECT_EQ(cgroups.get()[2], "/stu/grad");

  cgroups = cgroups::getCgroups(hierarchy, "/prof");
  ASSERT_SOME(cgroups);

  EXPECT_TRUE(cgroups.get().empty());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_GetTasks)
{
  Try<std::set<pid_t> > tasks = cgroups::getTasks(hierarchy, "/");
  ASSERT_SOME(tasks);

  std::set<pid_t> pids = tasks.get();
  EXPECT_NE(pids.find(1), pids.end());
  EXPECT_NE(pids.find(::getpid()), pids.end());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_ListenEvent)
{
  // Disable oom killer.
  ASSERT_SOME(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "memory.oom_control",
                                    "1"));

  // Limit the memory usage of "/prof" to 64MB.
  size_t limit = 1024 * 1024 * 64;
  ASSERT_SOME(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "memory.limit_in_bytes",
                                    stringify(limit)));

  // Listen on oom events for "/prof" cgroup.
  Future<uint64_t> future =
    cgroups::listenEvent(hierarchy,
                         "/prof",
                         "memory.oom_control");
  ASSERT_FALSE(future.isFailed());

  // Test the cancellation.
  future.discard();

  // Test the normal operation below.
  future = cgroups::listenEvent(hierarchy,
                                "/prof",
                                "memory.oom_control");
  ASSERT_FALSE(future.isFailed());

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid) {
    // In parent process.
    future.await(Seconds(5.0));

    EXPECT_TRUE(future.isReady());

    // Kill the child process.
    EXPECT_NE(-1, ::kill(pid, SIGKILL));

    // Wait for the child process.
    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  } else {
    // In child process. We try to trigger an oom here.
    // Put self into the "/prof" cgroup.
    Try<Nothing> assignResult = cgroups::assignTask(hierarchy,
                                                    "/prof",
                                                    ::getpid());
    if (assignResult.isError()) {
      FAIL() << "Failed to assign cgroup: " << assignResult.error();
    }

    // Blow up the memory.
    size_t limit = 1024 * 1024 * 512;
    void* buffer = NULL;

    if (posix_memalign(&buffer, getpagesize(), limit) != 0) {
      FAIL() << "Failed to allocate page-aligned memory, posix_memalign: "
             << strerror(errno);
    }

    // We use mlock and memset here to make sure that the memory
    // actually gets paged in and thus accounted for.
    if (mlock(buffer, limit) != 0) {
      FAIL() << "Failed to lock memory, mlock: " << strerror(errno);
    }

    if (memset(buffer, 1, limit) != 0) {
      FAIL() << "Failed to fill memory, memset: " << strerror(errno);
    }

    // Should not reach here.
    FAIL() << "OOM does not happen!";
  }
}


TEST_F(CgroupsTest, ROOT_CGROUPS_Freezer)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid) {
    // In parent process.
    ::close(pipes[1]);

    // Wait until child has assigned the cgroup.
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    // Freeze the "/prof" cgroup.
    Future<bool> freeze = cgroups::freezeCgroup(hierarchy, "/prof");
    freeze.await(Seconds(5.0));
    ASSERT_TRUE(freeze.isReady());
    EXPECT_EQ(true, freeze.get());

    // Thaw the "/prof" cgroup.
    Future<bool> thaw = cgroups::thawCgroup(hierarchy, "/prof");
    thaw.await(Seconds(5.0));
    ASSERT_TRUE(thaw.isReady());
    EXPECT_EQ(true, thaw.get());

    // Kill the child process.
    ASSERT_NE(-1, ::kill(pid, SIGKILL));

    // Wait for the child process.
    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  } else {
    // In child process.
    close(pipes[0]);

    // Put self into the "/prof" cgroup.
    Try<Nothing> assign = cgroups::assignTask(hierarchy,
                                              "/prof",
                                              ::getpid());
    if (assign.isError()) {
      FAIL() << "Failed to assign cgroup: " << assign.error();
    }

    // Notify the parent.
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      FAIL() << "Failed to notify the parent";
    }
    ::close(pipes[1]);

    // Infinite loop here.
    while (true) ;

    // Should not reach here.
    FAIL() << "Reach an unreachable statement!";
  }
}


TEST_F(CgroupsTest, ROOT_CGROUPS_KillTasks)
{
  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid) {
    // In parent process.
    ::close(pipes[1]);

    // Wait until all children have assigned the cgroup.
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    Future<bool> future = cgroups::killTasks(hierarchy, "/prof");
    future.await(Seconds(5.0));
    ASSERT_TRUE(future.isReady());
    EXPECT_TRUE(future.get());

    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  } else {
    // In child process.
    ::fork();
    ::fork();

    // Put self into "/prof" cgroup.
    Try<Nothing> assign = cgroups::assignTask(hierarchy, "/prof", ::getpid());
    if (assign.isError()) {
      FAIL() << "Failed to assign cgroup: " << assign.error();
    }

    // Notify the parent.
    ::close(pipes[0]);
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      FAIL() << "Failed to notify the parent";
    }
    ::close(pipes[1]);

    // Wait kill signal from parent.
    while (true) ;

    // Should not reach here.
    FAIL() << "Reach an unreachable statement!";
  }
}


TEST_F(CgroupsTest, ROOT_CGROUPS_DestroyCgroup)
{
  Future<bool> future = cgroups::destroyCgroup(hierarchy, "/stu/under");
  future.await(Seconds(5.0));
  ASSERT_TRUE(future.isReady());
  EXPECT_TRUE(future.get());

  int pipes[2];
  int dummy;
  ASSERT_NE(-1, ::pipe(pipes));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid) {
    // In parent process.
    ::close(pipes[1]);

    // Wait until all children have assigned the cgroup.
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ASSERT_NE(-1, ::read(pipes[0], &dummy, sizeof(dummy)));
    ::close(pipes[0]);

    Future<bool> future = cgroups::destroyCgroup(hierarchy, "/");
    future.await(Seconds(5.0));
    ASSERT_TRUE(future.isReady());
    EXPECT_TRUE(future.get());

    int status;
    EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  } else {
    // In child process.
    // We create 4 child processes here using two forks to test the case in
    // which there are multiple active processes in the given cgroup.
    ::fork();
    ::fork();

    // Put self into "/prof" cgroup.
    Try<Nothing> assign = cgroups::assignTask(hierarchy, "/prof", ::getpid());
    if (assign.isError()) {
      FAIL() << "Failed to assign cgroup: " << assign.error();
    }

    // Notify the parent.
    ::close(pipes[0]);
    if (::write(pipes[1], &dummy, sizeof(dummy)) != sizeof(dummy)) {
      FAIL() << "Failed to notify the parent";
    }
    ::close(pipes[1]);

    // Wait kill signal from parent.
    while (true) ;

    // Should not reach here.
    FAIL() << "Reach an unreachable statement!";
  }
}
