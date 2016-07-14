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

#include <stdarg.h>
#include <stdint.h>

#include <iostream>
#include <list>
#include <string>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <stout/gtest.hpp>

#include "master/allocator/sorter/drf/sorter.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::allocator::DRFSorter;

using ::testing::WithParamInterface;

using std::cout;
using std::endl;
using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


TEST(SorterTest, DRFSorter)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(slaveId, totalResources);

  sorter.add("a");
  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  sorter.allocated("a", slaveId, aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.add("b");
  sorter.allocated("b", slaveId, bResources);

  // shares: a = .05, b = .06
  EXPECT_EQ(list<string>({"a", "b"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  sorter.add("c");
  sorter.allocated("c", slaveId, cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1").get();
  sorter.add("d");
  sorter.allocated("d", slaveId, dResources);

  // shares: a = .05, b = .06, c = .01, d = .03
  EXPECT_EQ(list<string>({"c", "d", "a", "b"}), sorter.sort());

  sorter.remove("a");
  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b", slaveId, bUnallocated);

  // shares: b = .02, c = .01, d = .03
  EXPECT_EQ(list<string>({"c", "b", "d"}), sorter.sort());

  Resources eResources = Resources::parse("cpus:1;mem:5").get();
  sorter.add("e");
  sorter.allocated("e", slaveId, eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(slaveId, removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b = .04, c = .02, d = .06, e = .05
  EXPECT_EQ(list<string>({"c", "b", "e", "d"}), sorter.sort());

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(slaveId, addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1").get();
  sorter.add("f");
  sorter.allocated("f", slaveId, fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15").get();
  sorter.allocated("c", slaveId, cResources2);

  // shares: b = .04, c = .08, d = .06, e = .025, f = .1
  EXPECT_EQ(list<string>({"e", "b", "d", "c", "f"}), sorter.sort());

  EXPECT_TRUE(sorter.contains("b"));

  EXPECT_FALSE(sorter.contains("a"));

  EXPECT_EQ(5, sorter.count());

  sorter.deactivate("d");

  EXPECT_TRUE(sorter.contains("d"));

  EXPECT_EQ(list<string>({"e", "b", "c", "f"}), sorter.sort());

  EXPECT_EQ(5, sorter.count());

  sorter.activate("d");

  EXPECT_EQ(list<string>({"e", "b", "d", "c", "f"}), sorter.sort());
}


TEST(SorterTest, WDRFSorter)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");

  sorter.allocated("a", slaveId, Resources::parse("cpus:5;mem:5").get());

  sorter.add("b", 2);
  sorter.allocated("b", slaveId, Resources::parse("cpus:6;mem:6").get());

  // shares: a = .05, b = .03
  EXPECT_EQ(list<string>({"b", "a"}), sorter.sort());

  sorter.add("c");
  sorter.allocated("c", slaveId, Resources::parse("cpus:4;mem:4").get());

  // shares: a = .05, b = .03, c = .04
  EXPECT_EQ(list<string>({"b", "c", "a"}), sorter.sort());

  sorter.add("d", 10);
  sorter.allocated("d", slaveId, Resources::parse("cpus:10;mem:20").get());

  // shares: a = .05, b = .03, c = .04, d = .02
  EXPECT_EQ(list<string>({"d", "b", "c", "a"}), sorter.sort());

  sorter.remove("b");

  EXPECT_EQ(list<string>({"d", "c", "a"}), sorter.sort());

  sorter.allocated("d", slaveId, Resources::parse("cpus:10;mem:25").get());

  // shares: a = .05, c = .04, d = .045
  EXPECT_EQ(list<string>({"c", "d", "a"}), sorter.sort());

  sorter.add("e", .1);
  sorter.allocated("e", slaveId, Resources::parse("cpus:1;mem:1").get());

  // shares: a = .05, c = .04, d = .045, e = .1
  EXPECT_EQ(list<string>({"c", "d", "a", "e"}), sorter.sort());

  sorter.remove("a");

  EXPECT_EQ(list<string>({"c", "d", "e"}), sorter.sort());
}


TEST(SorterTest, WDRFSorterUpdateWeight)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();

  sorter.add(slaveId, totalResources);

  sorter.add("a");
  sorter.allocated("a", slaveId, Resources::parse("cpus:5;mem:5").get());

  sorter.add("b");
  sorter.allocated("b", slaveId, Resources::parse("cpus:6;mem:6").get());

  // shares: a = .05, b = .06
  EXPECT_EQ(list<string>({"a", "b"}), sorter.sort());

  // Increase b's  weight to flip the sort order.
  sorter.update("b", 2);

  // shares: a = .05, b = .03
  EXPECT_EQ(list<string>({"b", "a"}), sorter.sort());
}


// Some resources are split across multiple resource objects (e.g.
// persistent volumes). This test ensures that the shares for these
// are accounted correctly.
TEST(SorterTest, SplitResourceShares)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a");
  sorter.add("b");

  Resource disk1 = Resources::parse("disk", "5", "*").get();
  disk1.mutable_disk()->mutable_persistence()->set_id("ID2");
  disk1.mutable_disk()->mutable_volume()->set_container_path("data");

  Resource disk2 = Resources::parse("disk", "5", "*").get();
  disk2.mutable_disk()->mutable_persistence()->set_id("ID2");
  disk2.mutable_disk()->mutable_volume()->set_container_path("data");

  sorter.add(
      slaveId,
      Resources::parse("cpus:100;mem:100;disk:95").get() + disk1 + disk2);

  // Now, allocate resources to "a" and "b". Note that "b" will have
  // more disk if the shares are accounted correctly!
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:9;mem:9;disk:9").get());
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:9;mem:9").get() + disk1 + disk2);

  EXPECT_EQ(list<string>({"a", "b"}), sorter.sort());
}


TEST(SorterTest, UpdateAllocation)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a");
  sorter.add("b");

  sorter.add(slaveId, Resources::parse("cpus:10;mem:10;disk:10").get());

  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10;disk:10").get());

  // Construct an offer operation.
  Resource volume = Resources::parse("disk", "5", "*").get();
  volume.mutable_disk()->mutable_persistence()->set_id("ID");
  volume.mutable_disk()->mutable_volume()->set_container_path("data");

  // Compute the updated allocation.
  Resources oldAllocation = sorter.allocation("a", slaveId);
  Try<Resources> newAllocation = oldAllocation.apply(CREATE(volume));
  ASSERT_SOME(newAllocation);

  // Update the resources for the client.
  sorter.update("a", slaveId, oldAllocation, newAllocation.get());

  hashmap<SlaveID, Resources> allocation = sorter.allocation("a");
  EXPECT_EQ(1u, allocation.size());
  EXPECT_EQ(newAllocation.get(), allocation[slaveId]);
  EXPECT_EQ(newAllocation.get(), sorter.allocation("a", slaveId));
}


// We aggregate resources from multiple slaves into the sorter.
// Since non-scalar resources don't aggregate well across slaves,
// we need to keep track of the SlaveIDs of the resources. This
// tests that no resources vanish in the process of aggregation
// by inspecting the result of 'allocation'.
TEST(SorterTest, MultipleSlaves)
{
  DRFSorter sorter;

  SlaveID slaveA;
  slaveA.set_value("agentA");

  SlaveID slaveB;
  slaveB.set_value("agentB");

  sorter.add("framework");

  Resources slaveResources =
    Resources::parse("cpus:2;mem:512;ports:[31000-32000]").get();

  sorter.add(slaveA, slaveResources);
  sorter.add(slaveB, slaveResources);

  sorter.allocated("framework", slaveA, slaveResources);
  sorter.allocated("framework", slaveB, slaveResources);

  EXPECT_EQ(2u, sorter.allocation("framework").size());
  EXPECT_EQ(slaveResources, sorter.allocation("framework", slaveA));
  EXPECT_EQ(slaveResources, sorter.allocation("framework", slaveB));
}


// We aggregate resources from multiple slaves into the sorter. Since
// non-scalar resources don't aggregate well across slaves, we need to
// keep track of the SlaveIDs of the resources. This tests that no
// resources vanish in the process of aggregation by performing update
// allocations from unreserved to reserved resources.
TEST(SorterTest, MultipleSlavesUpdateAllocation)
{
  DRFSorter sorter;

  SlaveID slaveA;
  slaveA.set_value("agentA");

  SlaveID slaveB;
  slaveB.set_value("agentB");

  sorter.add("framework");

  Resources slaveResources =
    Resources::parse("cpus:2;mem:512;disk:10;ports:[31000-32000]").get();

  sorter.add(slaveA, slaveResources);
  sorter.add(slaveB, slaveResources);

  sorter.allocated("framework", slaveA, slaveResources);
  sorter.allocated("framework", slaveB, slaveResources);

  // Construct an offer operation.
  Resource volume = Resources::parse("disk", "5", "*").get();
  volume.mutable_disk()->mutable_persistence()->set_id("ID");
  volume.mutable_disk()->mutable_volume()->set_container_path("data");

  // Compute the updated allocation.
  Try<Resources> newAllocation = slaveResources.apply(CREATE(volume));
  ASSERT_SOME(newAllocation);

  // Update the resources for the client.
  sorter.update("framework", slaveA, slaveResources, newAllocation.get());
  sorter.update("framework", slaveB, slaveResources, newAllocation.get());

  EXPECT_EQ(2u, sorter.allocation("framework").size());
  EXPECT_EQ(newAllocation.get(), sorter.allocation("framework", slaveA));
  EXPECT_EQ(newAllocation.get(), sorter.allocation("framework", slaveB));
}


// This test verifies that when the total pool of resources is updated
// the sorting order of clients reflects the new total.
TEST(SorterTest, UpdateTotal)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a");
  sorter.add("b");

  sorter.add(slaveId, Resources::parse("cpus:10;mem:100").get());

  // Dominant share of "a" is 0.2 (cpus).
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:2;mem:1").get());

  // Dominant share of "b" is 0.1 (cpus).
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:1;mem:2").get());

  list<string> sorted = sorter.sort();
  ASSERT_EQ(2u, sorted.size());
  EXPECT_EQ("b", sorted.front());
  EXPECT_EQ("a", sorted.back());

  // Update the total resources by removing the previous total and
  // adding back the new total.
  sorter.remove(slaveId, Resources::parse("cpus:10;mem:100").get());
  sorter.add(slaveId, Resources::parse("cpus:100;mem:10").get());

  // Now the dominant share of "a" is 0.1 (mem) and "b" is 0.2 (mem),
  // which should change the sort order.
  sorted = sorter.sort();
  ASSERT_EQ(2u, sorted.size());
  EXPECT_EQ("a", sorted.front());
  EXPECT_EQ("b", sorted.back());
}


// Similar to the above 'UpdateTotal' test, but tests the scenario
// when there are multiple slaves.
TEST(SorterTest, MultipleSlavesUpdateTotal)
{
  DRFSorter sorter;

  SlaveID slaveA;
  slaveA.set_value("agentA");

  SlaveID slaveB;
  slaveB.set_value("agentB");

  sorter.add("a");
  sorter.add("b");

  sorter.add(slaveA, Resources::parse("cpus:5;mem:50").get());
  sorter.add(slaveB, Resources::parse("cpus:5;mem:50").get());

  // Dominant share of "a" is 0.2 (cpus).
  sorter.allocated(
      "a", slaveA, Resources::parse("cpus:2;mem:1").get());

  // Dominant share of "b" is 0.1 (cpus).
  sorter.allocated(
      "b", slaveB, Resources::parse("cpus:1;mem:3").get());

  list<string> sorted = sorter.sort();
  ASSERT_EQ(2u, sorted.size());
  EXPECT_EQ("b", sorted.front());
  EXPECT_EQ("a", sorted.back());

  // Update the total resources of slaveA by removing the previous
  // total and adding the new total.
  sorter.remove(slaveA, Resources::parse("cpus:5;mem:50").get());
  sorter.add(slaveA, Resources::parse("cpus:95;mem:50").get());

  // Now the dominant share of "a" is 0.02 (cpus) and "b" is 0.03
  // (mem), which should change the sort order.
  sorted = sorter.sort();
  ASSERT_EQ(2u, sorted.size());
  EXPECT_EQ("a", sorted.front());
  EXPECT_EQ("b", sorted.back());
}


// This test verifies that revocable resources are properly accounted
// for in the DRF sorter.
TEST(SorterTest, RevocableResources)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a");
  sorter.add("b");

  // Create a total resource pool of 10 revocable cpus and 10 cpus and
  // 100 MB mem.
  Resource revocable = Resources::parse("cpus", "10", "*").get();
  revocable.mutable_revocable();
  Resources total = Resources::parse("cpus:10;mem:100").get() + revocable;

  sorter.add(slaveId, total);

  // Dominant share of "a" is 0.1 (cpus).
  Resources a = Resources::parse("cpus:2;mem:1").get();
  sorter.allocated("a", slaveId, a);

  // Dominant share of "b" is 0.5 (cpus).
  revocable = Resources::parse("cpus", "9", "*").get();
  revocable.mutable_revocable();
  Resources b = Resources::parse("cpus:1;mem:1").get() + revocable;
  sorter.allocated("b", slaveId, b);

  // Check that the allocations are correct.
  ASSERT_EQ(a, sorter.allocation("a", slaveId));
  ASSERT_EQ(b, sorter.allocation("b", slaveId));

  // Check that the sort is correct.
  list<string> sorted = sorter.sort();
  ASSERT_EQ(2u, sorted.size());
  EXPECT_EQ("a", sorted.front());
  EXPECT_EQ("b", sorted.back());
}


class Sorter_BENCHMARK_Test
  : public ::testing::Test,
    public ::testing::WithParamInterface<std::tr1::tuple<size_t, size_t>> {};


// The sorter benchmark tests are parameterized by
// the number of clients and agents.
INSTANTIATE_TEST_CASE_P(
    AgentAndClientCount,
    Sorter_BENCHMARK_Test,
    ::testing::Combine(
      ::testing::Values(1000U, 5000U, 10000U, 20000U, 30000U, 50000U),
      ::testing::Values(1U, 50U, 100U, 200U, 500U, 1000U))
    );


// Returns a "ports" resource with the number of ranges
// specified as: [1-2, 4-5, 7-8, 10-11, ...]
static Resource makePortRanges(size_t numRanges)
{
  ::mesos::Value::Ranges ranges;

  for (size_t i = 0; i < numRanges; ++i) {
    Value::Range* range = ranges.add_range();
    range->set_begin((3 * i) + 1);
    range->set_end(range->begin() + 1);
  }

  Value value;
  value.set_type(Value::RANGES);
  value.mutable_ranges()->CopyFrom(ranges);

  Resource resource;
  resource.set_role("*");
  resource.set_name("ports");
  resource.set_type(Value::RANGES);
  resource.mutable_ranges()->CopyFrom(value.ranges());

  return resource;
}


// This benchmark simulates sorting a number of clients that have
// different amount of allocations.
TEST_P(Sorter_BENCHMARK_Test, FullSort)
{
  size_t agentCount = std::tr1::get<0>(GetParam());
  size_t clientCount = std::tr1::get<1>(GetParam());

  cout << "Using " << agentCount << " agents and "
       << clientCount << " clients" << endl;

  vector<SlaveID> agents;
  agents.reserve(agentCount);

  vector<string> clients;
  clients.reserve(clientCount);

  DRFSorter sorter;
  Stopwatch watch;

  watch.start();
  {
    for (size_t i = 0; i < clientCount; i++) {
      const string clientId = stringify(i);

      clients.push_back(clientId);

      sorter.add(clientId);
    }
  }
  watch.stop();

  cout << "Added " << clientCount << " clients in "
       << watch.elapsed() << endl;

  Resources agentResources = Resources::parse(
      "cpus:24;mem:4096;disk:4096;ports:[31000-32000]").get();

  watch.start();
  {
    for (size_t i = 0; i < agentCount; i++) {
      SlaveID slaveId;
      slaveId.set_value("agent" + stringify(i));

      agents.push_back(slaveId);

      sorter.add(slaveId, agentResources);
    }
  }
  watch.stop();

  cout << "Added " << agentCount << " agents in "
       << watch.elapsed() << endl;

  Resources allocated = Resources::parse(
      "cpus:16;mem:2014;disk:1024").get();

  allocated += makePortRanges(100);

  watch.start();
  {
    // Allocate resources on all agents, round-robin through the clients.
    size_t clientIndex = 0;
    foreach (const SlaveID& slaveId, agents) {
      const string& client = clients[clientIndex++ % clients.size()];
      sorter.allocated(client, slaveId, allocated);
    }
  }
  watch.stop();

  cout << "Added allocations for " << agentCount << " agents in "
         << watch.elapsed() << endl;

  watch.start();
  {
    sorter.sort();
  }
  watch.stop();

  cout << "Full sort of " << clientCount << " clients took "
       << watch.elapsed() << endl;

  watch.start();
  {
    sorter.sort();
  }
  watch.stop();

  cout << "No-op sort of " << clientCount << " clients took "
       << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
