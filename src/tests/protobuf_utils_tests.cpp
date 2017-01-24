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

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include "common/protobuf_utils.hpp"

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// This tests that helper function `getRoles` can correctly
// get roles from multi-role FrameworkInfo and role from
// single-role FrameworkInfo.
TEST(ProtobufUtilTest, GetRoles)
{
  // Get roles from a multi-role framework.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");

    set<string> roles = protobuf::framework::getRoles(frameworkInfo);

    EXPECT_EQ(roles, set<string>({"qux", "bar"}));
  }

  // Get role from a single-role framework.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_role("foo");

    set<string> roles = protobuf::framework::getRoles(frameworkInfo);

    EXPECT_EQ(roles, set<string>({"foo"}));
  }
}


// This tests that Capabilities are correctly constructed
// from given FrameworkInfo Capabilities.
TEST(ProtobufUtilTest, FrameworkCapabilities)
{
  auto toTypeSet = [](
      const protobuf::framework::Capabilities& capabilities) {
    set<FrameworkInfo::Capability::Type> result;

    if (capabilities.revocableResources) {
      result.insert(FrameworkInfo::Capability::REVOCABLE_RESOURCES);
    }
    if (capabilities.taskKillingState) {
      result.insert(FrameworkInfo::Capability::TASK_KILLING_STATE);
    }
    if (capabilities.gpuResources) {
      result.insert(FrameworkInfo::Capability::GPU_RESOURCES);
    }
    if (capabilities.sharedResources) {
      result.insert(FrameworkInfo::Capability::SHARED_RESOURCES);
    }
    if (capabilities.partitionAware) {
      result.insert(FrameworkInfo::Capability::PARTITION_AWARE);
    }
    if (capabilities.multiRole) {
      result.insert(FrameworkInfo::Capability::MULTI_ROLE);
    }

    return result;
  };

  auto typeSetToCapabilityVector = [](
      const set<FrameworkInfo::Capability::Type>& capabilitiesTypes) {
    vector<FrameworkInfo::Capability> result;

    foreach (FrameworkInfo::Capability::Type type, capabilitiesTypes) {
      FrameworkInfo::Capability capability;
      capability.set_type(type);

      result.push_back(capability);
    }

    return result;
  };

  // We test the `Capabilities` construction by converting back
  // to types and checking for equality with the original types.
  auto backAndForth = [=](const set<FrameworkInfo::Capability::Type>& types) {
    protobuf::framework::Capabilities capabilities(
        typeSetToCapabilityVector(types));

    return toTypeSet(capabilities);
  };

  set<FrameworkInfo::Capability::Type> expected;

  expected = { FrameworkInfo::Capability::REVOCABLE_RESOURCES };
  EXPECT_EQ(expected, backAndForth(expected));

  expected = { FrameworkInfo::Capability::TASK_KILLING_STATE };
  EXPECT_EQ(expected, backAndForth(expected));

  expected = { FrameworkInfo::Capability::GPU_RESOURCES };
  EXPECT_EQ(expected, backAndForth(expected));

  expected = { FrameworkInfo::Capability::SHARED_RESOURCES };
  EXPECT_EQ(expected, backAndForth(expected));

  expected = { FrameworkInfo::Capability::PARTITION_AWARE };
  EXPECT_EQ(expected, backAndForth(expected));

  expected = { FrameworkInfo::Capability::MULTI_ROLE };
  EXPECT_EQ(expected, backAndForth(expected));
}


// This tests that Capabilities are correctly constructed
// from given SlaveInfo Capabilities.
TEST(ProtobufUtilTest, AgentCapabilities)
{
  // TODO(jay_guo): consider applying the same test style in
  // FrameworkCapabilities when we have more capabilities in agent.
  SlaveInfo slaveInfo;
  slaveInfo.add_capabilities()->set_type(SlaveInfo::Capability::MULTI_ROLE);

  protobuf::slave::Capabilities capabilities =
    protobuf::slave::Capabilities(slaveInfo.capabilities());

  ASSERT_TRUE(capabilities.multiRole);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
