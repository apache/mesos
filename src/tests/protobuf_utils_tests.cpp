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

#include "tests/mesos.hpp"

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


// Tests that offer operations can be adjusted to include
// the appropriate allocation info, if not already set.
TEST(ProtobufUtilTest, AdjustAllocationInfoInOfferOperation)
{
  Resources resources = Resources::parse("cpus:1").get();

  Resources allocatedResources = resources;
  allocatedResources.allocate("role");

  Resource::AllocationInfo allocationInfo;
  allocationInfo.set_role("role");

  // Test the LAUNCH case. This should be constructing a valid
  // task and executor, but for now this just sets the resources
  // in order to verify the allocation info injection.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(resources);

  TaskInfo taskInfo;
  taskInfo.mutable_resources()->CopyFrom(resources);
  taskInfo.mutable_executor()->CopyFrom(executorInfo);

  Offer::Operation launch = LAUNCH({taskInfo});
  protobuf::adjustOfferOperation(&launch, allocationInfo);

  ASSERT_EQ(1, launch.launch().task_infos_size());

  EXPECT_EQ(allocatedResources,
            launch.launch().task_infos(0).resources());

  EXPECT_EQ(allocatedResources,
            launch.launch().task_infos(0).executor().resources());

  // Test the LAUNCH_GROUP case. This should be constructing a valid
  // task and executor, but for now this just sets the resources in
  // order to verify the allocation info injection.
  TaskGroupInfo taskGroupInfo;
  taskGroupInfo.add_tasks()->CopyFrom(taskInfo);

  Offer::Operation launchGroup = LAUNCH_GROUP(executorInfo, taskGroupInfo);
  protobuf::adjustOfferOperation(&launchGroup, allocationInfo);

  ASSERT_EQ(1, launchGroup.launch_group().task_group().tasks_size());

  EXPECT_EQ(allocatedResources,
            launchGroup.launch_group().task_group().tasks(0).resources());

  EXPECT_EQ(allocatedResources,
            launchGroup.launch_group().task_group().tasks(0).executor()
              .resources());

  // Test the RESERVE case. This should be constructing a valid
  // reservation, but for now this just sets the resources in
  // order to verify the allocation info injection.
  Offer::Operation reserve = RESERVE(resources);
  protobuf::adjustOfferOperation(&reserve, allocationInfo);

  EXPECT_EQ(allocatedResources, reserve.reserve().resources());

  // Test the UNRESERVE case. This should be constructing a valid
  // reservation, but for now this just sets the resources in
  // order to verify the allocation info injection.
  Offer::Operation unreserve = UNRESERVE(resources);
  protobuf::adjustOfferOperation(&unreserve, allocationInfo);

  EXPECT_EQ(allocatedResources, unreserve.unreserve().resources());

  // Test the CREATE case. This should be constructing a valid
  // volume, but for now this just sets the resources in order
  // to verify the allocation info injection.
  Offer::Operation create = CREATE(resources);
  protobuf::adjustOfferOperation(&create, allocationInfo);

  EXPECT_EQ(allocatedResources, create.create().volumes());

  // Test the DESTROY case. This should be constructing a valid
  // volume, but for now this just sets the resources in order
  // to verify the allocation info injection.
  Offer::Operation destroy = DESTROY(resources);
  protobuf::adjustOfferOperation(&destroy, allocationInfo);

  EXPECT_EQ(allocatedResources, destroy.destroy().volumes());
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
// from given Agent Capabilities.
TEST(ProtobufUtilTest, AgentCapabilities)
{
  // TODO(jay_guo): consider applying the same test style in
  // FrameworkCapabilities when we have more capabilities in agent.
  RegisterSlaveMessage registerSlaveMessage;
  registerSlaveMessage.add_agent_capabilities()->set_type(
      SlaveInfo::Capability::MULTI_ROLE);

  protobuf::slave::Capabilities capabilities(
      registerSlaveMessage.agent_capabilities());

  ASSERT_TRUE(capabilities.multiRole);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
