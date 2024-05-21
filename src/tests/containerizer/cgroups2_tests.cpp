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

#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/bytes.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/set.hpp>
#include <stout/tests/utils.hpp>
#include <stout/try.hpp>

#include "linux/cgroups2.hpp"
#include "linux/ebpf.hpp"

#include "tests/containerizer/memory_test_helper.hpp"

using process::Future;

using std::pair;
using std::set;
using std::string;
using std::thread;
using std::tuple;
using std::unique_ptr;
using std::vector;

namespace cpu = cgroups2::cpu;
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
        to_enable);

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
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
    }
  }

  void TearDown() override
  {
    if (cgroups2::exists(TEST_CGROUP)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
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

  // Should fetch the `pid` from the nested `TEST_CGROUP` if `recursive=true`.
  Try<set<pid_t>> root_pids = cgroups2::processes(cgroups2::ROOT_CGROUP, true);
  EXPECT_SOME(pids);
  EXPECT_EQ(1u, root_pids->count(pid));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, process::reap(pid));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_Threads)
{
  const size_t NUM_THREADS = 5;

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    vector<unique_ptr<std::thread>> runningThreads(NUM_THREADS);

    for (size_t i = 0; i < NUM_THREADS; ++i) {
      runningThreads[i] = unique_ptr<std::thread>(new std::thread([]() {
        os::sleep(Seconds(10));
      }));
    }

    // Check the test cgroup is initially empty.
    Try<set<pid_t>> cgroupThreads = cgroups2::threads(TEST_CGROUP);
    if (cgroupThreads.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to read threads in the cgroup");
    }
    if (!cgroupThreads->empty()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected no threads in the cgroup");
    }

    // Assign ourselves to the test cgroup.
    if (cgroups2::assign(TEST_CGROUP, ::getpid()).isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign to the test cgroup");
    }

    Try<set<pid_t>> threads = proc::threads(::getpid());
    if (threads.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to read threads");
    }

    // Check that the test cgroup now only contains all child threads.
    cgroupThreads = cgroups2::threads(TEST_CGROUP);
    if (cgroupThreads.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to read threads in the cgroup");
    }
    if (*threads != *cgroupThreads) {
      SAFE_EXIT(
          EXIT_FAILURE,
          "The threads in the cgroup did not match the processes's threads");
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
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


TEST_F(Cgroups2Test, ROOT_CGROUPS2_CpuBandwidthLimit)
{
  ASSERT_SOME(enable_controllers({"cpu"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"cpu"}));

  Try<cpu::BandwidthLimit> limit = cgroups2::cpu::max(TEST_CGROUP);
  ASSERT_SOME(limit);
  EXPECT_NONE(limit->limit); // The default limit is limitless.

  cpu::BandwidthLimit new_limit =
    cpu::BandwidthLimit(Microseconds(10000), Microseconds(20000));
  EXPECT_SOME(cgroups2::cpu::set_max(TEST_CGROUP, new_limit));

  limit = cgroups2::cpu::max(TEST_CGROUP);
  ASSERT_SOME(limit);
  EXPECT_EQ(new_limit.limit, limit->limit);
  EXPECT_EQ(new_limit.period, limit->period);

  new_limit = cpu::BandwidthLimit();
  EXPECT_SOME(cgroups2::cpu::set_max(TEST_CGROUP, new_limit));

  limit = cgroups2::cpu::max(TEST_CGROUP);
  ASSERT_SOME(limit);
  EXPECT_EQ(new_limit.limit, limit->limit);
  EXPECT_EQ(new_limit.period, limit->period);
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryUsage)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  // Does not exist for the root cgroup.
  EXPECT_ERROR(cgroups2::memory::usage(cgroups2::ROOT_CGROUP));

  EXPECT_SOME(cgroups2::memory::usage(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryStats)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));
  ASSERT_SOME(cgroups2::memory::stats(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryLow)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  const Bytes bytes = Bytes(os::pagesize()) * 5;

  // Does not exist for the root cgroup.
  EXPECT_ERROR(cgroups2::memory::low(cgroups2::ROOT_CGROUP));
  EXPECT_ERROR(cgroups2::memory::set_low(cgroups2::ROOT_CGROUP, bytes));

  EXPECT_SOME(cgroups2::memory::set_low(TEST_CGROUP, bytes));
  EXPECT_SOME_EQ(bytes, cgroups2::memory::low(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryMinimum)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  const Bytes bytes = Bytes(os::pagesize()) * 5;

  // Does not exist for the root cgroup.
  EXPECT_ERROR(cgroups2::memory::min(cgroups2::ROOT_CGROUP));
  EXPECT_ERROR(cgroups2::memory::set_min(cgroups2::ROOT_CGROUP, bytes));

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes));
  EXPECT_SOME_EQ(bytes, cgroups2::memory::min(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryMaximum)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  Bytes limit = Bytes(os::pagesize()) * 5;

  // Does not exist for the root cgroup.
  EXPECT_ERROR(cgroups2::memory::max(cgroups2::ROOT_CGROUP));
  EXPECT_ERROR(cgroups2::memory::set_max(cgroups2::ROOT_CGROUP, limit));

  EXPECT_SOME(cgroups2::memory::set_max(TEST_CGROUP, limit));
  EXPECT_SOME_EQ(limit, cgroups2::memory::max(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_max(TEST_CGROUP, None()));
  EXPECT_NONE(cgroups2::memory::max(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemorySoftMaximum)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  Bytes limit = Bytes(os::pagesize()) * 5;

  // Does not exist for the root cgroup.
  EXPECT_ERROR(cgroups2::memory::high(cgroups2::ROOT_CGROUP));
  EXPECT_ERROR(cgroups2::memory::set_high(cgroups2::ROOT_CGROUP, limit));

  EXPECT_SOME(cgroups2::memory::set_high(TEST_CGROUP, limit));
  EXPECT_SOME_EQ(limit, cgroups2::memory::high(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_high(TEST_CGROUP, None()));
  EXPECT_NONE(cgroups2::memory::high(TEST_CGROUP));
}


// Check that byte amounts written to the memory controller are rounded
// down to the nearest page size.
TEST_F(Cgroups2Test, ROOT_CGROUPS2_MemoryBytesRounding)
{
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  const Bytes bytes = Bytes(os::pagesize());

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes - 1));
  EXPECT_SOME_EQ(Bytes(0), cgroups2::memory::min(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes + 1));
  EXPECT_SOME_EQ(bytes, cgroups2::memory::min(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes * 5 - 1));
  EXPECT_SOME_EQ(bytes * 4, cgroups2::memory::min(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes * 5));
  EXPECT_SOME_EQ(bytes * 5, cgroups2::memory::min(TEST_CGROUP));

  EXPECT_SOME(cgroups2::memory::set_min(TEST_CGROUP, bytes * 5 + 1));
  EXPECT_SOME_EQ(bytes * 5, cgroups2::memory::min(TEST_CGROUP));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_GetCgroups)
{
  vector<string> cgroups = {
    path::join(TEST_CGROUP, "test1"),
    path::join(TEST_CGROUP, "test1/a"),
    path::join(TEST_CGROUP, "test1/b"),
    path::join(TEST_CGROUP, "test1/b/c"),
    path::join(TEST_CGROUP, "test2"),
    path::join(TEST_CGROUP, "test2/a"),
    path::join(TEST_CGROUP, "test2/a/b"),
    path::join(TEST_CGROUP, "test3"),
  };

  foreach (const string& cgroup, cgroups) {
    ASSERT_SOME(cgroups2::create(cgroup, true));
  }

  EXPECT_SOME_EQ(
      set<string>({
        path::join(TEST_CGROUP, "test1"),
        path::join(TEST_CGROUP, "test1/a"),
        path::join(TEST_CGROUP, "test1/b"),
        path::join(TEST_CGROUP, "test1/b/c"),
        path::join(TEST_CGROUP, "test2"),
        path::join(TEST_CGROUP, "test2/a"),
        path::join(TEST_CGROUP, "test2/a/b"),
        path::join(TEST_CGROUP, "test3"),
      }),
      cgroups2::get(TEST_CGROUP));

  EXPECT_SOME_EQ(
      set<string>({
        path::join(TEST_CGROUP, "test1/a"),
        path::join(TEST_CGROUP, "test1/b"),
        path::join(TEST_CGROUP, "test1/b/c"),
      }),
      cgroups2::get(path::join(TEST_CGROUP, "test1")));

  EXPECT_SOME_EQ(set<string>({
        path::join(TEST_CGROUP, "test2/a"),
        path::join(TEST_CGROUP, "test2/a/b"),
      }),
      cgroups2::get(path::join(TEST_CGROUP, "test2")));

  EXPECT_SOME_EQ(
      set<string>({
        path::join(TEST_CGROUP, "test1/b/c")
      }),
      cgroups2::get(path::join(TEST_CGROUP, "test1/b")));
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_OomDetection)
{
  // Check that exceeding the hard memory limit will trigger an OOM event.
  //
  // We set a hard memory limit on the test cgroup, move a process inside of
  // the test cgroup subtree, listen for an OOM event, deliberately trigger an
  // OOM event by exceeding the hard limit, and then check that an event was
  // triggered.
  ASSERT_SOME(enable_controllers({"memory"}));

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  ASSERT_SOME(cgroups2::controllers::enable(TEST_CGROUP, {"memory"}));

  const string leaf_cgroup = TEST_CGROUP + "/leaf";
  ASSERT_SOME(cgroups2::create(leaf_cgroup));

  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  Bytes limit = Megabytes(64);

  ASSERT_SOME(cgroups2::assign(leaf_cgroup, *helper.pid()));
  ASSERT_SOME(cgroups2::memory::set_max(TEST_CGROUP, limit));

  Future<Nothing> oomEvent = cgroups2::memory::oom(TEST_CGROUP);

  // Assert that the OOM event has not been triggered.
  EXPECT_FALSE(oomEvent.isReady());

  // Increase memory usage beyond the limit.
  ASSERT_ERROR(helper.increaseRSS(limit * 2));

  AWAIT_EXPECT_READY(oomEvent);
}


// Arguments for os::open(). Combination of a path and an access type.
typedef pair<string, int> OpenArgs;

using DeviceControllerTestParams = tuple<
    vector<devices::Entry>,
    vector<devices::Entry>,
    vector<OpenArgs>,
    vector<OpenArgs>>;

class DeviceControllerTestFixture :
    public Cgroups2Test,
    public ::testing::WithParamInterface<DeviceControllerTestParams> {};


TEST_P(DeviceControllerTestFixture, ROOT_CGROUPS2_DeviceController)
{
  const string& cgroup = TEST_CGROUP;

  auto params = GetParam();
  const vector<devices::Entry>& allow = std::get<0>(params);
  const vector<devices::Entry>& deny = std::get<1>(params);
  const vector<OpenArgs>& allowed_accesses = std::get<2>(params);
  const vector<OpenArgs>& blocked_accesses = std::get<3>(params);

  ASSERT_SOME(cgroups2::create(cgroup));
  string path = cgroups2::path(cgroup);

  Try<vector<uint32_t>> attached = ebpf::cgroups2::attached(path);
  ASSERT_SOME(attached);
  ASSERT_EQ(0u, attached->size());

  ASSERT_SOME(devices::configure(cgroup, allow, deny));
  attached = ebpf::cgroups2::attached(path);
  ASSERT_SOME(attached);
  ASSERT_EQ(1u, attached->size());

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Move the child process into the newly created cgroup.
    Try<Nothing> assign = cgroups2::assign(cgroup, ::getpid());
    if (assign.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign child process to cgroup");
    }

    // Check that we can only do the "allowed_accesses".
    foreach(const OpenArgs& args, allowed_accesses) {
      if (os::open(args.first, args.second).isError()) {
        SAFE_EXIT(EXIT_FAILURE, "Expected allowed read to succeed");
      }
    }
    foreach(const OpenArgs& args, blocked_accesses) {
      if (os::open(args.first, args.second).isSome()) {
        SAFE_EXIT(EXIT_FAILURE, "Expected blocked read to fail");
      }
    }

    ASSERT_SOME(ebpf::cgroups2::detach(path, attached->at(0)));

    // Check that we can do both the "allowed_accesses" and "blocked_accesses".
    foreach(const OpenArgs& args, allowed_accesses) {
      if (os::open(args.first, args.second).isError()) {
        SAFE_EXIT(EXIT_FAILURE, "Expected successful read after detaching"
          " device controller program");
      }
    }
    foreach(const OpenArgs& args, blocked_accesses) {
      if (os::open(args.first, args.second).isError()) {
        SAFE_EXIT(EXIT_FAILURE, "Expected successful read after detaching"
          " device controller program");
      }
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
}


INSTANTIATE_TEST_CASE_P(
    DeviceControllerTestParams,
    DeviceControllerTestFixture,
    ::testing::Values<DeviceControllerTestParams>(
        // Acceses are blocked by default if no entries are configured.
        DeviceControllerTestParams{
          vector<devices::Entry>{},
          vector<devices::Entry>{},
          vector<OpenArgs>{},
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}, {os::DEV_NULL, O_RDWR}}
        },
        // Deny access to /dev/null.
        DeviceControllerTestParams{
          vector<devices::Entry>{},
          vector<devices::Entry>{*devices::Entry::parse("c 1:3 rwm")},
          vector<OpenArgs>{},
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}}
        },
        // Allow access to /dev/null.
        DeviceControllerTestParams{
          vector<devices::Entry>{*devices::Entry::parse("c 1:3 rwm")},
          vector<devices::Entry>{},
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}},
          vector<OpenArgs>{}
        },
        // Allow read-only access to /dev/null.
        DeviceControllerTestParams{
          vector<devices::Entry>{*devices::Entry::parse("c 1:3 r")},
          vector<devices::Entry>{},
          vector<OpenArgs>{{os::DEV_NULL, O_RDONLY}},
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}}
        },
        // Allow write-only access to /dev/null.
        DeviceControllerTestParams{
          vector<devices::Entry>{*devices::Entry::parse("c 1:3 w")},
          vector<devices::Entry>{},
            // write-only allowed
          vector<OpenArgs>{{os::DEV_NULL, O_WRONLY}},
            // read is blocked
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}, {os::DEV_NULL, O_RDONLY}}
        },
        // Access to /dev/null is denied because the allowed access is to
        // a different device type with the same major:minor numbers.
        DeviceControllerTestParams{
          vector<devices::Entry>{*devices::Entry::parse("b 1:3 rwm")},
          vector<devices::Entry>{},
          vector<OpenArgs>{},
            // /dev/null is blocked
          vector<OpenArgs>{{os::DEV_NULL, O_RDWR}, {os::DEV_NULL, O_RDONLY}}
        },
        // Allow access to all devices.
        DeviceControllerTestParams{
          vector<devices::Entry>{*devices::Entry::parse("a *:* rwm")},
          vector<devices::Entry>{},
          vector<OpenArgs>{
            {os::DEV_NULL, O_WRONLY},
            {os::DEV_NULL, O_RDWR},
            {os::DEV_NULL, O_RDONLY}},
          vector<OpenArgs>{},
        }
      ));

} // namespace tests {

} // namespace internal {

} // namespace mesos {

