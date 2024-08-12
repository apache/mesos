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

#include "common/protobuf_utils.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>
#include <tuple>

#include <process/gtest.hpp>
#include <process/reap.hpp>

#include <stout/unreachable.hpp>
#include <stout/tests/utils.hpp>

#include "linux/cgroups2.hpp"
#include "slave/containerizer/device_manager/device_manager.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using mesos::internal::slave::DeviceManager;
using mesos::internal::slave::DeviceManagerProcess;

using process::Future;
using process::Owned;

using std::string;
using std::tuple;
using std::vector;

namespace devices = cgroups2::devices;

namespace mesos {
namespace internal {
namespace tests {

const string TEST_CGROUP = "test";

const string TEST_CGROUPS_ROOT = "mesos_test";

const string TEST_CGROUP_WITH_ROOT = path::join(TEST_CGROUPS_ROOT, TEST_CGROUP);


class DeviceManagerTest : public TemporaryDirectoryTest
{
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    // Cleanup the test cgroup, in case a previous test run didn't clean it
    // up properly.
    if (cgroups2::exists(TEST_CGROUP)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
    }

    if (cgroups2::exists(TEST_CGROUP_WITH_ROOT)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP_WITH_ROOT));
    }
  }

  void TearDown() override
  {
    if (cgroups2::exists(TEST_CGROUP)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
    }

    if (cgroups2::exists(TEST_CGROUP_WITH_ROOT)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP_WITH_ROOT));
    }

    TemporaryDirectoryTest::TearDown();
  }
};


TEST(NonWildcardEntry, NonWildcardFromWildcard)
{
  EXPECT_ERROR(DeviceManager::NonWildcardEntry::create(
      vector<devices::Entry>{*devices::Entry::parse("c *:1 w")}));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_Normal)
{
  typedef std::pair<string, int> OpenArgs;
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 r")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 w")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Move the child process into the newly created cgroup.
    Try<Nothing> assign = cgroups2::assign(TEST_CGROUP, ::getpid());
    if (assign.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign child process to cgroup");
    }

    // Check that we can only do the "allowed_accesses".
    if (os::open(os::DEV_NULL, O_RDONLY).isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected allowed read to succeed");
    }
    if (os::open(os::DEV_NULL, O_RDWR).isSome()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected blocked read to fail");
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerReconfigure_Normal)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 w")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  vector<devices::Entry> additions = {*devices::Entry::parse("c 1:3 r")};
  vector<devices::Entry> removals = allow_list;

  AWAIT_ASSERT_READY(dm->reconfigure(
      TEST_CGROUP,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(additions)),
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(removals))));

  cgroup_state = dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(additions, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Move the child process into the newly created cgroup.
    Try<Nothing> assign = cgroups2::assign(TEST_CGROUP, ::getpid());
    if (assign.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign child process to cgroup");
    }

    // Check that we can only do the "allowed_accesses".
    if (os::open(os::DEV_NULL, O_RDONLY).isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected allowed read to succeed");
    }
    if (os::open(os::DEV_NULL, O_RDWR).isSome()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected blocked read to fail");
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_AllowMatchesDeny)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> deny_list = {
    *devices::Entry::parse("c 1:3 w"),
    *devices::Entry::parse("c 21:1 w")
  };

  AWAIT_ASSERT_FAILED(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(deny_list))));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_AllowWildcard)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("a *:* m")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 m")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerGetDiffState_AllowMatchesDeny)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> additions = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> removals = {
    *devices::Entry::parse("c 1:3 w"),
    *devices::Entry::parse("c 21:1 w")
  };

  AWAIT_ASSERT_FAILED(dm->reconfigure(
      TEST_CGROUP,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(additions)),
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(removals))));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerRemove)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 w")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  Future<Nothing> removal = dm->remove(TEST_CGROUP);
  AWAIT_ASSERT_READY(removal);

  Future<hashmap<std::string, DeviceManager::CgroupDeviceAccess>> dm_state =
    dm->state();
  EXPECT_FALSE(dm_state->contains(TEST_CGROUP));

  cgroup_state = dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(vector<devices::Entry>{}, cgroup_state->allow_list);
  EXPECT_EQ(vector<devices::Entry>{}, cgroup_state->deny_list);
}


using DeviceManagerGetDiffStateTestParams = tuple<
  vector<devices::Entry>, // Allow list for initial configure.
  vector<devices::Entry>, // Deny list for initial configure.
  vector<devices::Entry>, // Additions for reconfigure.
  vector<devices::Entry>, // Removals for reconfigure.
  vector<devices::Entry>, // Expected allow list after reconfigure.
  vector<devices::Entry>  // Expected deny list after reconfigure.
>;


class DeviceManagerGetDiffStateTestFixture
  : public DeviceManagerTest,
    public ::testing::WithParamInterface<DeviceManagerGetDiffStateTestParams>
{};


TEST_P(DeviceManagerGetDiffStateTestFixture, ROOT_DeviceManagerGetDiffState)
{
  auto params = GetParam();
  vector<devices::Entry> setup_allow = std::get<0>(params);
  vector<devices::Entry> setup_deny = std::get<1>(params);
  vector<devices::Entry> additions = std::get<2>(params);
  vector<devices::Entry> removals = std::get<3>(params);
  vector<devices::Entry> reconfigured_allow = std::get<4>(params);
  vector<devices::Entry> reconfigured_deny = std::get<5>(params);

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      setup_allow,
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(setup_deny))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(setup_allow, cgroup_state->allow_list);
  EXPECT_EQ(setup_deny, cgroup_state->deny_list);

  cgroup_state = dm->apply_diff(
      cgroup_state.get(),
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(additions)),
      CHECK_NOTERROR(DeviceManager::NonWildcardEntry::create(removals)));

  EXPECT_EQ(reconfigured_allow, cgroup_state->allow_list);
  EXPECT_EQ(reconfigured_deny, cgroup_state->deny_list);
}


INSTANTIATE_TEST_CASE_P(
  DeviceManagerGetDiffStateTestParams,
  DeviceManagerGetDiffStateTestFixture,
  ::testing::Values<DeviceManagerGetDiffStateTestParams>(
    // Remove existing allow entry accesses:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 w")},
      vector<devices::Entry>{}
    },
    // Remove existing deny entry accesses:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rwm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rwm")
      },
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 w")}
    },
    // Remove entire existing allow entry:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{}
    },
    // Remove entire existing deny entry:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rm")
      },
      vector<devices::Entry>{}
    },
    // Overlapping entries where none encompasses the other:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rw")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rm"),
        *devices::Entry::parse("c 3:1 rw")
      },
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 m")}
    },
    // Overlapping with non-encompassing wildcard:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rw")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 r")}
    }));


TEST(DeviceManagerCgroupDeviceAccessTest, IsAccessGrantedTest)
{
  // Character devices with major minor numbrs 1:3 can do write only:
  DeviceManager::CgroupDeviceAccess cgroup_device_access =
    CHECK_NOTERROR(DeviceManager::CgroupDeviceAccess::create(
        {*devices::Entry::parse("c 1:3 w")}, {}
    ));
  EXPECT_TRUE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 1:3 w"))
  );
  EXPECT_FALSE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 1:3 rw"))
  );
  EXPECT_FALSE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("b 1:3 w"))
  );

  // Character devices with minor number 3 can do write only:
  cgroup_device_access = CHECK_NOTERROR(
      DeviceManager::CgroupDeviceAccess::create(
      {*devices::Entry::parse("c *:3 w")}, {}
  ));
  EXPECT_TRUE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 4:3 w"))
  );
  EXPECT_TRUE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c *:3 w"))
  );

  // Character devices with major number 5 can do write only:
  cgroup_device_access = CHECK_NOTERROR(
      DeviceManager::CgroupDeviceAccess::create(
      {(*devices::Entry::parse("c 5:* w"))}, {}
  ));
  EXPECT_TRUE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 5:* w"))
  );
  EXPECT_TRUE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 5:2 w"))
  );

  // All devices will match the catch-all and can perform all operations:
  cgroup_device_access = CHECK_NOTERROR(
      DeviceManager::CgroupDeviceAccess::create(
      {*devices::Entry::parse("a *:* rwm")}, {}
  ));
  EXPECT_TRUE(
    cgroup_device_access.is_access_granted(*devices::Entry::parse("c 6:2 w")));

  // Deny all accesses to character device with major and numbers 1:3.
  cgroup_device_access = CHECK_NOTERROR(
      DeviceManager::CgroupDeviceAccess::create(
      {}, {*devices::Entry::parse("c 1:3 rwm")}
  ));
  EXPECT_FALSE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 1:3 w"))
  );
  EXPECT_FALSE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 1:3 rw"))
  );

  // Entry should be denied if encompassed by an entry in allow_list and
  // overlaps with entry in deny_list.
  cgroup_device_access = CHECK_NOTERROR(
      DeviceManager::CgroupDeviceAccess::create(
      {*devices::Entry::parse("c 1:3 rw")}, {*devices::Entry::parse("c 1:3 w")}
  ));
  EXPECT_FALSE(
      cgroup_device_access.is_access_granted(*devices::Entry::parse("c 1:3 rw"))
  );
}


TEST_F(DeviceManagerTest, ROOT_Recover)
{
  slave::Flags flags;
  flags.work_dir = *sandbox;
  flags.cgroups_root = TEST_CGROUPS_ROOT;
  ASSERT_SOME(cgroups2::create(TEST_CGROUP_WITH_ROOT, true));
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};

  Future<Nothing> setup = dm->configure(
      TEST_CGROUP_WITH_ROOT,
      allow_list,
      {});

  AWAIT_ASSERT_READY(setup);

  Future<DeviceManager::CgroupDeviceAccess> dm_state =
    dm->state(TEST_CGROUP_WITH_ROOT);

  AWAIT_ASSERT_READY(dm_state);

  EXPECT_EQ(dm_state->allow_list, allow_list);
  EXPECT_EQ(dm_state->deny_list, vector<devices::Entry>{});

  dm = Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  dm_state = dm->state(TEST_CGROUP_WITH_ROOT);
  AWAIT_ASSERT_READY(dm_state);
  EXPECT_EQ(dm_state->allow_list, vector<devices::Entry>{});
  EXPECT_EQ(dm_state->deny_list, vector<devices::Entry>{});

  Option<ContainerID> container_id =
    slave::containerizer::paths::cgroups2::containerId(
        flags.cgroups_root, TEST_CGROUP);
  ASSERT_SOME(container_id);


  Future<Nothing> recover = dm->recover({protobuf::slave::createContainerState(
      None(), None(), *container_id, -1, *sandbox)});

  AWAIT_ASSERT_READY(recover);

  dm_state = dm->state(TEST_CGROUP_WITH_ROOT);
  AWAIT_ASSERT_READY(dm_state);
  EXPECT_EQ(dm_state->allow_list, allow_list);
  EXPECT_EQ(dm_state->deny_list, vector<devices::Entry>{});
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
