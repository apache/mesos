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

#include <errno.h>

#include <set>
#include <string>
#include <vector>

#include <process/gtest.hpp>
#include <process/subprocess.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "linux/capabilities.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/capabilities_test_helper.hpp"

using std::set;
using std::string;
using std::vector;

using process::Future;
using process::Subprocess;

using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;

namespace mesos {
namespace internal {
namespace tests {

constexpr char CAPS_TEST_UNPRIVILEGED_USER[] = "nobody";


class CapabilitiesTest : public ::testing::Test
{
public:
  // Launch 'ping' using the given capabilities and user.
  Try<Subprocess> ping(
      const set<Capability>& capabilities,
      const Option<string>& user = None())
  {
    CapabilitiesTestHelper helper;

    helper.flags.user = user;
    helper.flags.capabilities = capabilities::convert(capabilities);

    vector<string> argv = {
      "test-helper",
      CapabilitiesTestHelper::NAME
    };

    return subprocess(
        getTestHelperPath("test-helper"),
        argv,
        Subprocess::FD(STDIN_FILENO),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO),
        &helper.flags);
  }
};


// This test verifies that an operation ('ping') that needs `NET_RAW`
// capability does not succeed if the capability `NET_RAW` is dropped.
TEST_F(CapabilitiesTest, ROOT_PingWithNoNetRawCaps)
{
  Try<Capabilities> manager = Capabilities::create();
  ASSERT_SOME(manager);

  Try<ProcessCapabilities> capabilities = manager->get();
  ASSERT_SOME(capabilities);

  capabilities->drop(capabilities::PERMITTED, capabilities::NET_RAW);

  Try<Subprocess> s = ping(capabilities->get(capabilities::PERMITTED));
  ASSERT_SOME(s);

  AWAIT_EXPECT_WEXITSTATUS_NE(0, s->status());
}


// This test verifies that the effective capabilities of a process can
// be controlled after `setuid` system call. An operation ('ping')
// that needs `NET_RAW` capability does not succeed if the capability
// `NET_RAW` is dropped.
TEST_F(CapabilitiesTest, ROOT_PingWithNoNetRawCapsChangeUser)
{
  Try<Capabilities> manager = Capabilities::create();
  ASSERT_SOME(manager);

  Try<ProcessCapabilities> capabilities = manager->get();
  ASSERT_SOME(capabilities);

  capabilities->drop(capabilities::PERMITTED, capabilities::NET_RAW);

  Try<Subprocess> s = ping(
      capabilities->get(capabilities::PERMITTED),
      CAPS_TEST_UNPRIVILEGED_USER);

  ASSERT_SOME(s);

  AWAIT_EXPECT_WEXITSTATUS_NE(0, s->status());
}


// This Test verifies that 'ping' would work with just the minimum
// capability it requires ('NET_RAW' and potentially 'NET_ADMIN').
//
// NOTE: Some Linux distributions install `ping` with `NET_RAW` and
// `NET_ADMIN` in both the effective and permitted set in the file
// capabilities. We only require `NET_RAW` for our tests, while
// `NET_RAW` is needed for setting packet marks
// (https://bugzilla.redhat.com/show_bug.cgi?id=802197). In such
// distributions, setting 'NET_ADMIN' is required to bypass the
// 'capability-dumb' check by the kernel. A 'capability-dump'
// application is a traditional set-user-ID-root program that has been
// switched to use file capabilities, but whose code has not been
// modified to understand capabilities. For such applications, the
// kernel checks if the process obtained all permitted capabilities
// that were specified in the file permitted set during 'exec'.
TEST_F(CapabilitiesTest, ROOT_PingWithJustNetRawSysAdminCap)
{
  set<Capability> capabilities = {
    capabilities::NET_RAW,
    capabilities::NET_ADMIN
  };

  Try<Subprocess> s = ping(capabilities, CAPS_TEST_UNPRIVILEGED_USER);
  ASSERT_SOME(s);

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
