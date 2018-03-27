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
#include <mesos/type_utils.hpp>

#include <stout/stringify.hpp>

#include "common/protobuf_utils.hpp"

#include "tests/mesos.hpp"

using std::set;
using std::string;
using std::vector;

using google::protobuf::Map;

using mesos::internal::protobuf::convertLabelsToStringMap;
using mesos::internal::protobuf::convertStringMapToLabels;
using mesos::internal::protobuf::createLabel;

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


// Tests that allocation info can be injected to and stripped from
// offer operations.
TEST(ProtobufUtilTest, InjectAndStripAllocationInfoInOfferOperation)
{
  Resources unallocatedResources = Resources::parse("cpus:1").get();

  Resources allocatedResources = unallocatedResources;
  allocatedResources.allocate("role");

  Resource::AllocationInfo allocationInfo;
  allocationInfo.set_role("role");

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(unallocatedResources);

  TaskInfo taskInfo;
  taskInfo.mutable_resources()->CopyFrom(unallocatedResources);
  taskInfo.mutable_executor()->CopyFrom(executorInfo);

  {
    // Test the LAUNCH case. This should be constructing a valid
    // task and executor, but for now this just sets the resources
    // in order to verify the allocation info injection and stripping.
    Offer::Operation launch = LAUNCH({taskInfo});
    protobuf::injectAllocationInfo(&launch, allocationInfo);

    ASSERT_EQ(1, launch.launch().task_infos_size());

    EXPECT_EQ(allocatedResources,
              launch.launch().task_infos(0).resources());

    EXPECT_EQ(allocatedResources,
              launch.launch().task_infos(0).executor().resources());

    protobuf::stripAllocationInfo(&launch);

    EXPECT_EQ(unallocatedResources,
              launch.launch().task_infos(0).resources());

    EXPECT_EQ(unallocatedResources,
              launch.launch().task_infos(0).executor().resources());
  }

  {
    // Test the LAUNCH_GROUP case. This should be constructing a valid
    // task and executor, but for now this just sets the resources in
    // order to verify the allocation info injection.
    TaskGroupInfo taskGroupInfo;
    taskGroupInfo.add_tasks()->CopyFrom(taskInfo);

    Offer::Operation launchGroup = LAUNCH_GROUP(executorInfo, taskGroupInfo);
    protobuf::injectAllocationInfo(&launchGroup, allocationInfo);

    ASSERT_EQ(1, launchGroup.launch_group().task_group().tasks_size());

    EXPECT_EQ(allocatedResources,
              launchGroup.launch_group().task_group().tasks(0).resources());

    EXPECT_EQ(allocatedResources,
              launchGroup.launch_group().task_group().tasks(0).executor()
                .resources());

    protobuf::stripAllocationInfo(&launchGroup);

    EXPECT_EQ(unallocatedResources,
              launchGroup.launch_group().task_group().tasks(0).resources());

    EXPECT_EQ(unallocatedResources,
              launchGroup.launch_group().task_group().tasks(0).executor()
                .resources());
  }

  {
    // Test the RESERVE case. This should be constructing a valid
    // reservation, but for now this just sets the resources in
    // order to verify the allocation info injection.
    Offer::Operation reserve = RESERVE(unallocatedResources);
    protobuf::injectAllocationInfo(&reserve, allocationInfo);

    EXPECT_EQ(allocatedResources, reserve.reserve().resources());

    protobuf::stripAllocationInfo(&reserve);

    EXPECT_EQ(unallocatedResources, reserve.reserve().resources());
  }

  {
    // Test the UNRESERVE case. This should be constructing a valid
    // reservation, but for now this just sets the resources in
    // order to verify the allocation info injection.
    Offer::Operation unreserve = UNRESERVE(unallocatedResources);
    protobuf::injectAllocationInfo(&unreserve, allocationInfo);

    EXPECT_EQ(allocatedResources, unreserve.unreserve().resources());

    protobuf::stripAllocationInfo(&unreserve);

    EXPECT_EQ(unallocatedResources, unreserve.unreserve().resources());
  }

  {
    // Test the CREATE case. This should be constructing a valid
    // volume, but for now this just sets the resources in order
    // to verify the allocation info injection.
    Offer::Operation create = CREATE(unallocatedResources);
    protobuf::injectAllocationInfo(&create, allocationInfo);

    EXPECT_EQ(allocatedResources, create.create().volumes());

    protobuf::stripAllocationInfo(&create);

    EXPECT_EQ(unallocatedResources, create.create().volumes());
  }

  {
    // Test the DESTROY case. This should be constructing a valid
    // volume, but for now this just sets the resources in order
    // to verify the allocation info injection.
    Offer::Operation destroy = DESTROY(unallocatedResources);
    protobuf::injectAllocationInfo(&destroy, allocationInfo);

    EXPECT_EQ(allocatedResources, destroy.destroy().volumes());

    protobuf::stripAllocationInfo(&destroy);

    EXPECT_EQ(unallocatedResources, destroy.destroy().volumes());
  }
}


// This tests that helper function `convertLabelsToStringMap` can
// correctly convert a `Labels` to a protobuf string map and helper
// function `convertStringMapToLabels` can convert it back.
TEST(ProtobufUtilTest, ConvertBetweenLabelsAndStringMap)
{
  Labels labels1;
  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));

  Try<Map<string, string>> map1 = convertLabelsToStringMap(labels1);
  ASSERT_SOME(map1);
  ASSERT_NE(map1->end(), map1->find("foo"));
  EXPECT_EQ("bar", map1->at("foo"));

  EXPECT_EQ(labels1, convertStringMapToLabels(map1.get()));

  Labels labels2;
  labels2.add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  EXPECT_ERROR(convertLabelsToStringMap(labels2));

  Labels labels3;
  labels3.add_labels()->CopyFrom(createLabel("foo", None()));

  EXPECT_ERROR(convertLabelsToStringMap(labels3));
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
    if (capabilities.regionAware) {
      result.insert(FrameworkInfo::Capability::REGION_AWARE);
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

  expected = { FrameworkInfo::Capability::REGION_AWARE };
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


// Test large message evolve.
// Before protobuf 3.3.0, this test would fail due to the 64MB limit
// imposed by older version of protobuf.
TEST(ProtobufUtilTest, LargeMessageEvolve)
{
  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  while (Bytes(data.size()) < Megabytes(70)) {
    data.append(data);
  }

  ExecutorInfo executorInfo_;
  executorInfo_.set_data(data);

  evolve(executorInfo_);
}


TEST(ProtobufUtilTest, ParseContainerID)
{
  ContainerID parent;
  parent.set_value("parent");

  ContainerID child;
  child.set_value("child");
  child.mutable_parent()->CopyFrom(parent);

  EXPECT_EQ(parent, protobuf::parseContainerId(stringify(parent)));
  EXPECT_EQ(child, protobuf::parseContainerId(stringify(child)));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
