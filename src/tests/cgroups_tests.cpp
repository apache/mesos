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
    ASSERT_TRUE(cgroups::createHierarchy(hierarchy, subsystems).isSome());

    // Create cgroups for test.
    ASSERT_TRUE(cgroups::createCgroup(hierarchy, "prof").isSome());
    ASSERT_TRUE(cgroups::createCgroup(hierarchy, "stu").isSome());
    ASSERT_TRUE(cgroups::createCgroup(hierarchy, "stu/grad").isSome());
    ASSERT_TRUE(cgroups::createCgroup(hierarchy, "stu/under").isSome());
    ASSERT_TRUE(cgroups::createCgroup(hierarchy, "stu/under/senior").isSome());
  }

  void cleanup()
  {
    if (cgroups::checkHierarchy(hierarchy).isSome()) {
      // Remove all cgroups.
      Try<std::vector<std::string> > cgroups = cgroups::getCgroups(hierarchy);
      ASSERT_TRUE(cgroups.isSome());
      foreach (const std::string& cgroup, cgroups.get()) {
        ASSERT_TRUE(cgroups::removeCgroup(hierarchy, cgroup).isSome());
      }

      // Remove the hierarchy.
      ASSERT_TRUE(cgroups::removeHierarchy(hierarchy).isSome());
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
  Try<bool> result = false;

  result = cgroups::enabled("cpu");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::enabled(",cpu");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::enabled("cpu,memory");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::enabled("cpu,memory,");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::enabled("invalid");
  EXPECT_TRUE(result.isError());

  result = cgroups::enabled("cpu,invalid");
  EXPECT_TRUE(result.isError());

  result = cgroups::enabled(",");
  EXPECT_TRUE(result.isError());

  result = cgroups::enabled("");
  EXPECT_TRUE(result.isError());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_Busy)
{
  Try<bool> result = false;

  result = cgroups::busy("invalid");
  EXPECT_TRUE(result.isError());

  result = cgroups::busy("cpu,invalid");
  EXPECT_TRUE(result.isError());

  result = cgroups::busy(",");
  EXPECT_TRUE(result.isError());

  result = cgroups::busy("");
  EXPECT_TRUE(result.isError());

  result = cgroups::busy("cpu");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::busy(",cpu");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::busy("cpu,memory");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());

  result = cgroups::busy("cpu,memory,");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(result.get());
}


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_Subsystems)
{
  Try<std::set<std::string> > names = cgroups::subsystems();
  ASSERT_TRUE(names.isSome());

  Option<std::string> cpu;
  Option<std::string> memory;
  foreach (const std::string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }

  EXPECT_TRUE(cpu.isSome());
  EXPECT_TRUE(memory.isSome());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_SubsystemsHierarchy)
{
  Try<std::set<std::string> > names = cgroups::subsystems(hierarchy);
  ASSERT_TRUE(names.isSome());

  Option<std::string> cpu;
  Option<std::string> memory;
  foreach (const std::string& name, names.get()) {
    if (name == "cpu") {
      cpu = name;
    } else if (name == "memory") {
      memory = name;
    }
  }

  EXPECT_TRUE(cpu.isSome());
  EXPECT_TRUE(memory.isSome());
}


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_CreateRemoveHierarchy)
{
  EXPECT_TRUE(cgroups::createHierarchy("/tmp", "cpu").isError());

  EXPECT_TRUE(cgroups::createHierarchy(hierarchy, "invalid").isError());

  ASSERT_TRUE(cgroups::createHierarchy(hierarchy, "cpu,memory").isSome());

  EXPECT_TRUE(cgroups::createHierarchy(hierarchy, "cpuset").isError());

  EXPECT_TRUE(cgroups::removeHierarchy("/tmp").isError());

  ASSERT_TRUE(cgroups::removeHierarchy(hierarchy).isSome());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_CheckHierarchy)
{
  EXPECT_TRUE(cgroups::checkHierarchy("/tmp-nonexist").isError());

  EXPECT_TRUE(cgroups::checkHierarchy("/tmp").isError());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy).isSome());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy + "/").isSome());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy + "/stu").isError());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_CheckHierarchySubsystems)
{
  EXPECT_TRUE(cgroups::checkHierarchy("/tmp-nonexist", "cpu").isError());

  EXPECT_TRUE(cgroups::checkHierarchy("/tmp", "cpu,memory").isError());

  EXPECT_TRUE(cgroups::checkHierarchy("/tmp", "cpu").isError());

  EXPECT_TRUE(cgroups::checkHierarchy("/tmp", "invalid").isError());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy, "cpu,memory").isSome());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy, "memory").isSome());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy, "invalid").isError());

  EXPECT_TRUE(cgroups::checkHierarchy(hierarchy + "/stu", "cpu").isError());
}


TEST_F(CgroupsSimpleTest, ROOT_CGROUPS_CreateRemoveCgroup)
{
  EXPECT_TRUE(cgroups::createCgroup("/tmp", "test").isError());

  ASSERT_TRUE(cgroups::createHierarchy(hierarchy, "cpu,memory").isSome());

  EXPECT_TRUE(cgroups::createCgroup(hierarchy, "test/1").isError());

  ASSERT_TRUE(cgroups::createCgroup(hierarchy, "test").isSome());

  EXPECT_TRUE(cgroups::removeCgroup(hierarchy, "invalid").isError());

  ASSERT_TRUE(cgroups::removeCgroup(hierarchy, "test").isSome());

  ASSERT_TRUE(cgroups::removeHierarchy(hierarchy).isSome());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_ReadControl)
{
  Try<std::string> result = std::string();
  std::string pid = stringify(::getpid());

  result = cgroups::readControl(hierarchy, "/stu", "invalid");
  EXPECT_TRUE(result.isError());

  result = cgroups::readControl(hierarchy, "/", "tasks");
  ASSERT_TRUE(result.isSome());
  EXPECT_TRUE(strings::contains(result.get(), pid));
}


TEST_F(CgroupsTest, ROOT_CGROUPS_WriteControl)
{
  std::string pid = stringify(::getpid());

  EXPECT_TRUE(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "invalid",
                                    "invalid").isError());

  ASSERT_TRUE(cgroups::writeControl(hierarchy, "/prof", "tasks", pid).isSome());

  Try<std::set<pid_t> > tasks = cgroups::getTasks(hierarchy, "/prof");
  ASSERT_TRUE(tasks.isSome());

  std::set<pid_t> pids = tasks.get();
  EXPECT_NE(pids.find(::getpid()), pids.end());

  ASSERT_TRUE(cgroups::writeControl(hierarchy, "/", "tasks", pid).isSome());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_GetCgroups)
{
  Try<std::vector<std::string> > cgroups = cgroups::getCgroups(hierarchy);
  ASSERT_TRUE(cgroups.isSome());

  EXPECT_EQ(cgroups.get()[0], "/stu/under/senior");
  EXPECT_EQ(cgroups.get()[1], "/stu/under");
  EXPECT_EQ(cgroups.get()[2], "/stu/grad");
  EXPECT_EQ(cgroups.get()[3], "/stu");
  EXPECT_EQ(cgroups.get()[4], "/prof");

  cgroups = cgroups::getCgroups(hierarchy, "/stu");
  ASSERT_TRUE(cgroups.isSome());

  EXPECT_EQ(cgroups.get()[0], "/stu/under/senior");
  EXPECT_EQ(cgroups.get()[1], "/stu/under");
  EXPECT_EQ(cgroups.get()[2], "/stu/grad");

  cgroups = cgroups::getCgroups(hierarchy, "/prof");
  ASSERT_TRUE(cgroups.isSome());

  EXPECT_TRUE(cgroups.get().empty());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_GetTasks)
{
  Try<std::set<pid_t> > tasks = cgroups::getTasks(hierarchy, "/");
  ASSERT_TRUE(tasks.isSome());

  std::set<pid_t> pids = tasks.get();
  EXPECT_NE(pids.find(1), pids.end());
  EXPECT_NE(pids.find(::getpid()), pids.end());
}


TEST_F(CgroupsTest, ROOT_CGROUPS_ListenEvent)
{
  // Disable oom killer.
  ASSERT_TRUE(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "memory.oom_control",
                                    "1").isSome());

  // Limit the memory usage of "/prof" to 64MB.
  size_t limit = 1024 * 1024 * 64;
  ASSERT_TRUE(cgroups::writeControl(hierarchy,
                                    "/prof",
                                    "memory.limit_in_bytes",
                                    stringify(limit)).isSome());

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
