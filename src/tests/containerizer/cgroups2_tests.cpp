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

#include <set>
#include <stdlib.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <process/reap.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/set.hpp>
#include <stout/tests/utils.hpp>
#include <stout/try.hpp>

#include "linux/cgroups2.hpp"
#include "linux/ebpf.hpp"

using std::pair;
using std::set;
using std::string;
using std::tuple;
using std::vector;

namespace devices = cgroups2::devices;

namespace mesos {
namespace internal {
namespace tests {

const string TEST_CGROUP = "test";

// Cgroups2 test fixture that ensures `TEST_CGROUP` does not
// exist before and after the test is run.
class Cgroups2Test : public TemporaryDirectoryTest
{
protected:
  Try<Nothing> enable_controllers(const vector<string>& controllers)
  {
    Try<set<string>> enabled = cgroups2::controllers::enabled(
        cgroups2::ROOT_CGROUP);

    if (enabled.isError()) {
      return Error("Failed to check enabled controllers: " + enabled.error());
    }

    set<string> to_enable(controllers.begin(), controllers.end());
    to_enable = to_enable - *enabled;

    Try<Nothing> result = cgroups2::controllers::enable(
        cgroups2::ROOT_CGROUP,
        vector<string>(to_enable.begin(), to_enable.end()));

    if (result.isSome()) {
      enabled_controllers = enabled_controllers | to_enable;
    }

    return result;
  }

  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    // Cleanup the test cgroup, in case a previous test run didn't clean it
    // up properly.
    if (cgroups2::exists(TEST_CGROUP)) {
      ASSERT_SOME(cgroups2::destroy(TEST_CGROUP));
    }
  }

  void TearDown() override
  {
    if (cgroups2::exists(TEST_CGROUP)) {
      ASSERT_SOME(cgroups2::destroy(TEST_CGROUP));
    }

    ASSERT_SOME(cgroups2::controllers::disable(
        cgroups2::ROOT_CGROUP, enabled_controllers));

    TemporaryDirectoryTest::TearDown();
  }

  // These are controllers that *we* enabled, and therefore we need to
  // disable on test cleanup. Note that if the tests are killed, then
  // we'll still of course leak these side effects, unfortunately.
  set<string> enabled_controllers;
};


TEST_F(Cgroups2Test, ROOT_CGROUPS2_Enabled)
{
  EXPECT_TRUE(cgroups2::enabled());
}


TEST_F(Cgroups2Test, CGROUPS2_Path)
{
  EXPECT_EQ("/sys/fs/cgroup/", cgroups2::path(cgroups2::ROOT_CGROUP));
  EXPECT_EQ("/sys/fs/cgroup/foo", cgroups2::path("foo"));
  EXPECT_EQ("/sys/fs/cgroup/foo/bar", cgroups2::path("foo/bar"));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_AvailableSubsystems)
{

  Try<bool> mounted = cgroups2::mounted();
  ASSERT_SOME(mounted);

  if (!*mounted) {
    ASSERT_SOME(cgroups2::mount());
  }

  Try<set<string>> available = cgroups2::controllers::available(
    cgroups2::ROOT_CGROUP);

  ASSERT_SOME(available);
  EXPECT_TRUE(available->count("cpu") == 1);

  if (!*mounted) {
    EXPECT_SOME(cgroups2::unmount());
  }
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_AssignProcesses)
{
  Try<set<pid_t>> pids = cgroups2::processes(cgroups2::ROOT_CGROUP);

  EXPECT_SOME(pids);
  EXPECT_TRUE(pids->size() > 0);

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process, wait for kill signal.
    while (true) { sleep(1); }

    SAFE_EXIT(
        EXIT_FAILURE, "Error, child should be killed before reaching here");
  }

  pids = cgroups2::processes(TEST_CGROUP);
  EXPECT_SOME(pids);
  EXPECT_EQ(0u, pids->size());

  EXPECT_SOME(cgroups2::assign(TEST_CGROUP, pid));
  pids = cgroups2::processes(TEST_CGROUP);

  EXPECT_SOME(pids);
  EXPECT_EQ(1u, pids->size());
  EXPECT_EQ(pid, *pids->begin());

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, process::reap(pid));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_CpuStats)
{
  ASSERT_SOME(enable_controllers({"cpu"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"cpu"}));
  ASSERT_SOME(cgroups2::cpu::stats(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_EnableAndDisable)
{
  ASSERT_SOME(enable_controllers({"cpu"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));

  // Check that "cpu" not enabled.
  Try<set<string>> enabled = cgroups2::controllers::enabled(TEST_CGROUP);
  EXPECT_SOME(enabled);
  EXPECT_EQ(0u, enabled->count("cpu"));

  // Enable "cpu".
  EXPECT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"cpu"}));
  EXPECT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"cpu"})); // NOP

  // Check that "cpu" is enabled.
  enabled = cgroups2::controllers::enabled(TEST_CGROUP);
  EXPECT_SOME(enabled);
  EXPECT_EQ(1u, enabled->count("cpu"));

  // Disable "cpu".
  EXPECT_SOME(cgroups2::controllers::disable(TEST_CGROUP, {"cpu"}));
  EXPECT_SOME(cgroups2::controllers::disable(TEST_CGROUP, {"cpu"})); // NOP

  // Check that "cpu" not enabled.
  enabled = cgroups2::controllers::enabled(TEST_CGROUP);
  EXPECT_SOME(enabled);
  EXPECT_EQ(0u, enabled->count("cpu"));
}

// Combination of a path and access flags.
typedef pair<string, int> Access;

using DeviceControllerTestParams = tuple<
    vector<devices::Entry>, 
    vector<devices::Entry>,
    vector<Access>, 
    vector<Access>>;

class DeviceControllerTestFixture :
    public Cgroups2Test,
    public ::testing::WithParamInterface<DeviceControllerTestParams> {};


TEST_P(DeviceControllerTestFixture, ROOT_CGROUPS2_DeviceController) {
  const string& cgroup = TEST_CGROUP;

  auto params = GetParam();
  const vector<devices::Entry> allow = std::get<0>(params);
  const vector<devices::Entry> deny = std::get<1>(params);
  const vector<Access> allowedAccesses = std::get<2>(params);
  const vector<Access> blockedAccesses = std::get<3>(params);

  if (!cgroups2::exists(cgroup)) {
    ASSERT_SOME(cgroups2::create(cgroup));
  }
  string path = cgroups2::path(cgroup);

  ASSERT_SOME(devices::configure(cgroup, allow, deny));
  Try<vector<uint32_t>> attached = ebpf::cgroups2::attached(path);
  EXPECT_SOME(attached);
  EXPECT_EQ(1u, attached->size());

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Check that we can only do the "allowedAccesses".
    foreach(const Access& access, allowedAccesses) {
      ASSERT_SOME(os::open(access.first, access.second));
    }
    foreach(const Access& access, blockedAccesses) {
      ASSERT_ERROR(os::open(access.first, access.second));
    }

    ASSERT_SOME(ebpf::cgroups2::detach(path, attached->at(0)));

    // Check that we can do both the "allowedAccesses" and "blockedAccesses".
    foreach(const Access& access, allowedAccesses) {
      ASSERT_SOME(os::open(access.first, access.second));
    }
    foreach(const Access& access, blockedAccesses) {
      ASSERT_SOME(os::open(access.first, access.second));
    }

    // Wait for kill signal.
    while (true) { sleep(1); }

    SAFE_EXIT(
        EXIT_FAILURE, "Error, child should be killed before reaching here");
  }

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, process::reap(pid));
}


INSTANTIATE_TEST_CASE_P(
    DeviceControllerTestParams,
    DeviceControllerTestFixture,
    ::testing::Values<DeviceControllerTestParams>(
       DeviceControllerTestParams{
            vector<devices::Entry>{},
            vector<devices::Entry>{*devices::Entry::parse("c *:* rwm")},
            vector<Access>{},
            vector<Access>{{os::DEV_NULL, O_RDWR}}
        },
        DeviceControllerTestParams{
            // allow /dev/null
            vector<devices::Entry>{*devices::Entry::parse("c 1:3 rwm")}, 
            vector<devices::Entry>{},
            // read-write allowed
            vector<Access>{{os::DEV_NULL, O_RDWR}},                      
            vector<Access>{}
        },
        DeviceControllerTestParams{
            // allow /dev/null
            vector<devices::Entry>{*devices::Entry::parse("c 1:3 r")}, 
            vector<devices::Entry>{},
            // read-only allowed
            vector<Access>{{os::DEV_NULL, O_RDONLY}},                  
            // read-write is blocked
            vector<Access>{{os::DEV_NULL, O_RDWR}}                     
        },
        DeviceControllerTestParams{
            vector<devices::Entry>{*devices::Entry::parse("a 1:3 w")},
            vector<devices::Entry>{},
            // write-only allowed
            vector<Access>{{os::DEV_NULL, O_WRONLY}},                        
            // read is blocked
            vector<Access>{{os::DEV_NULL, O_RDWR}, {os::DEV_NULL, O_RDONLY}} 
        },
        DeviceControllerTestParams{
            vector<devices::Entry>{*devices::Entry::parse("b 1:3 r")},
            vector<devices::Entry>{},
            vector<Access>{},
            // /dev/null is blocked
            vector<Access>{{os::DEV_NULL, O_RDWR}, {os::DEV_NULL, O_RDONLY}} 
        }
      ));

} // namespace tests {

} // namespace internal {

} // namespace mesos {

