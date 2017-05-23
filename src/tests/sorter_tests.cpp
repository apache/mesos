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

#include <iostream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <stout/gtest.hpp>

#include "master/allocator/sorter/drf/sorter.hpp"

#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

using mesos::internal::master::allocator::DRFSorter;

using std::cout;
using std::endl;
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

  EXPECT_EQ(vector<string>({}), sorter.sort());

  sorter.add("a");
  sorter.activate("a");
  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  sorter.allocated("a", slaveId, aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.add("b");
  sorter.activate("b");
  sorter.allocated("b", slaveId, bResources);

  // shares: a = .05, b = .06
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  sorter.add("c");
  sorter.activate("c");
  sorter.allocated("c", slaveId, cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1").get();
  sorter.add("d");
  sorter.activate("d");
  sorter.allocated("d", slaveId, dResources);

  // shares: a = .05, b = .06, c = .01, d = .03
  EXPECT_EQ(vector<string>({"c", "d", "a", "b"}), sorter.sort());

  sorter.remove("a");

  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b", slaveId, bUnallocated);

  // shares: b = .02, c = .01, d = .03
  EXPECT_EQ(vector<string>({"c", "b", "d"}), sorter.sort());

  Resources eResources = Resources::parse("cpus:1;mem:5").get();
  sorter.add("e");
  sorter.activate("e");
  sorter.allocated("e", slaveId, eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(slaveId, removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b = .04, c = .02, d = .06, e = .05
  EXPECT_EQ(vector<string>({"c", "b", "e", "d"}), sorter.sort());

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(slaveId, addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1").get();
  sorter.add("f");
  sorter.activate("f");
  sorter.allocated("f", slaveId, fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15").get();
  sorter.allocated("c", slaveId, cResources2);

  // shares: b = .04, c = .08, d = .06, e = .025, f = .1
  EXPECT_EQ(vector<string>({"e", "b", "d", "c", "f"}), sorter.sort());

  EXPECT_TRUE(sorter.contains("b"));

  EXPECT_FALSE(sorter.contains("a"));

  EXPECT_EQ(5, sorter.count());

  sorter.deactivate("d");

  EXPECT_TRUE(sorter.contains("d"));

  EXPECT_EQ(vector<string>({"e", "b", "c", "f"}), sorter.sort());

  EXPECT_EQ(5, sorter.count());

  sorter.activate("d");

  EXPECT_EQ(vector<string>({"e", "b", "d", "c", "f"}), sorter.sort());
}


TEST(SorterTest, WDRFSorter)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");
  sorter.activate("a");

  sorter.allocated("a", slaveId, Resources::parse("cpus:5;mem:5").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.updateWeight("b", 2);
  sorter.allocated("b", slaveId, Resources::parse("cpus:6;mem:6").get());

  // shares: a = .05, b = .03
  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  sorter.add("c");
  sorter.activate("c");
  sorter.allocated("c", slaveId, Resources::parse("cpus:4;mem:4").get());

  // shares: a = .05, b = .03, c = .04
  EXPECT_EQ(vector<string>({"b", "c", "a"}), sorter.sort());

  sorter.add("d");
  sorter.activate("d");
  sorter.updateWeight("d", 10);
  sorter.allocated("d", slaveId, Resources::parse("cpus:10;mem:20").get());

  // shares: a = .05, b = .03, c = .04, d = .02
  EXPECT_EQ(vector<string>({"d", "b", "c", "a"}), sorter.sort());

  sorter.remove("b");

  EXPECT_EQ(vector<string>({"d", "c", "a"}), sorter.sort());

  sorter.allocated("d", slaveId, Resources::parse("cpus:10;mem:25").get());

  // shares: a = .05, c = .04, d = .045
  EXPECT_EQ(vector<string>({"c", "d", "a"}), sorter.sort());

  sorter.add("e");
  sorter.activate("e");
  sorter.updateWeight("e", 0.1);
  sorter.allocated("e", slaveId, Resources::parse("cpus:1;mem:1").get());

  // shares: a = .05, c = .04, d = .045, e = .1
  EXPECT_EQ(vector<string>({"c", "d", "a", "e"}), sorter.sort());

  sorter.remove("a");

  EXPECT_EQ(vector<string>({"c", "d", "e"}), sorter.sort());
}


TEST(SorterTest, WDRFSorterUpdateWeight)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();

  sorter.add(slaveId, totalResources);

  sorter.add("a");
  sorter.activate("a");
  sorter.allocated("a", slaveId, Resources::parse("cpus:5;mem:5").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated("b", slaveId, Resources::parse("cpus:6;mem:6").get());

  // shares: a = .05, b = .06
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());

  // Increase b's weight to flip the sort order.
  sorter.updateWeight("b", 2);

  // shares: a = .05, b = .03
  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());
}


// Check that the sorter uses the total number of allocations made to
// a client as a tiebreaker when the two clients have the same share.
TEST(SorterTest, CountAllocations)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();

  sorter.add(slaveId, totalResources);

  sorter.add("a");
  sorter.add("b");
  sorter.add("c");
  sorter.add("d");
  sorter.add("e");
  sorter.activate("a");
  sorter.activate("b");
  sorter.activate("c");
  sorter.activate("d");
  sorter.activate("e");

  // Everyone is allocated the same amount of resources; "c" gets
  // three distinct allocations, "d" gets two, and all other clients
  // get one.
  sorter.allocated("a", slaveId, Resources::parse("cpus:3;mem:3").get());
  sorter.allocated("b", slaveId, Resources::parse("cpus:3;mem:3").get());
  sorter.allocated("c", slaveId, Resources::parse("cpus:1;mem:1").get());
  sorter.allocated("c", slaveId, Resources::parse("cpus:1;mem:1").get());
  sorter.allocated("c", slaveId, Resources::parse("cpus:1;mem:1").get());
  sorter.allocated("d", slaveId, Resources::parse("cpus:2;mem:2").get());
  sorter.allocated("d", slaveId, Resources::parse("cpus:1;mem:1").get());
  sorter.allocated("e", slaveId, Resources::parse("cpus:3;mem:3").get());

  // Allocation count: {a,b,e} = 1, {d} = 2, {c} = 3.
  EXPECT_EQ(vector<string>({"a", "b", "e", "d", "c"}), sorter.sort());

  // Check that unallocating and re-allocating to a client does not
  // reset the allocation count.
  sorter.unallocated("c", slaveId, Resources::parse("cpus:3;mem:3").get());

  EXPECT_EQ(vector<string>({"c", "a", "b", "e", "d"}), sorter.sort());

  sorter.allocated("c", slaveId, Resources::parse("cpus:3;mem:3").get());

  // Allocation count: {a,b,e} = 1, {d} = 2, {c} = 4.
  EXPECT_EQ(vector<string>({"a", "b", "e", "d", "c"}), sorter.sort());

  // Check that deactivating and then re-activating a client does not
  // reset the allocation count.
  sorter.deactivate("c");
  sorter.activate("c");

  // Allocation count: {a,b,e} = 1, {d} = 2, {c} = 4.
  EXPECT_EQ(vector<string>({"a", "b", "e", "d", "c"}), sorter.sort());

  sorter.unallocated("c", slaveId, Resources::parse("cpus:3;mem:3").get());
  sorter.allocated("c", slaveId, Resources::parse("cpus:3;mem:3").get());

  // Allocation count: {a,b,e} = 1, {d} = 2, {c} = 5.
  EXPECT_EQ(vector<string>({"a", "b", "e", "d", "c"}), sorter.sort());

  // Check that allocations to an inactive client increase the
  // allocation count.
  sorter.deactivate("a");

  sorter.unallocated("a", slaveId, Resources::parse("cpus:1;mem:3").get());
  sorter.allocated("a", slaveId, Resources::parse("cpus:1;mem:3").get());

  // Allocation count: {b,e} = 1, {d} = 2, {c} = 5.
  EXPECT_EQ(vector<string>({"b", "e", "d", "c"}), sorter.sort());

  sorter.activate("a");

  // Allocation count: {b,e} = 1, {a,d} = 2, {c} = 5.
  EXPECT_EQ(vector<string>({"b", "e", "a", "d", "c"}), sorter.sort());
}


// This test checks a simple case of hierarchical allocation: the same
// sequence of operations happens as in the `DRFSorter` test, but all
// client names are nested into disjoint branches of the tree. In this
// case, the hierarchy should not change allocation behavior.
TEST(SorterTest, ShallowHierarchy)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(slaveId, totalResources);

  sorter.add("a/a");
  sorter.activate("a/a");

  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  sorter.allocated("a/a", slaveId, aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.add("b/b");
  sorter.activate("b/b");
  sorter.allocated("b/b", slaveId, bResources);

  // shares: a/a = .05, b/b = .06
  EXPECT_EQ(vector<string>({"a/a", "b/b"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  sorter.add("c/c");
  sorter.activate("c/c");
  sorter.allocated("c/c", slaveId, cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1").get();
  sorter.add("d/d");
  sorter.activate("d/d");
  sorter.allocated("d/d", slaveId, dResources);

  // shares: a/a = .05, b/b = .06, c/c = .01, d/d = .03
  EXPECT_EQ(vector<string>({"c/c", "d/d", "a/a", "b/b"}), sorter.sort());

  sorter.remove("a/a");

  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b/b", slaveId, bUnallocated);

  // shares: b/b = .02, c/c = .01, d/d = .03
  EXPECT_EQ(vector<string>({"c/c", "b/b", "d/d"}), sorter.sort());

  Resources eResources = Resources::parse("cpus:1;mem:5").get();
  sorter.add("e/e");
  sorter.activate("e/e");
  sorter.allocated("e/e", slaveId, eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(slaveId, removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b/b = .04, c/c = .02, d/d = .06, e/e = .05
  EXPECT_EQ(vector<string>({"c/c", "b/b", "e/e", "d/d"}), sorter.sort());

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(slaveId, addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1").get();
  sorter.add("f/f");
  sorter.activate("f/f");
  sorter.allocated("f/f", slaveId, fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15").get();
  sorter.allocated("c/c", slaveId, cResources2);

  // shares: b = .04, c = .08, d = .06, e = .025, f = .1
  EXPECT_EQ(vector<string>({"e/e", "b/b", "d/d", "c/c", "f/f"}), sorter.sort());

  EXPECT_TRUE(sorter.contains("b/b"));

  EXPECT_FALSE(sorter.contains("a/a"));

  EXPECT_EQ(5, sorter.count());

  sorter.deactivate("d/d");

  EXPECT_TRUE(sorter.contains("d/d"));

  EXPECT_EQ(vector<string>({"e/e", "b/b", "c/c", "f/f"}), sorter.sort());

  EXPECT_EQ(5, sorter.count());

  sorter.activate("d/d");

  EXPECT_EQ(vector<string>({"e/e", "b/b", "d/d", "c/c", "f/f"}), sorter.sort());
}


// Analogous to `ShallowHierarchy` except the client names are nested
// more deeply and different client names are at different depths in
// the tree.
TEST(SorterTest, DeepHierarchy)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(slaveId, totalResources);

  sorter.add("a/a/a/a/a");
  sorter.activate("a/a/a/a/a");
  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  sorter.allocated("a/a/a/a/a", slaveId, aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.add("b/b/b/b");
  sorter.activate("b/b/b/b");
  sorter.allocated("b/b/b/b", slaveId, bResources);

  // shares: a/a/a/a/a = .05, b/b/b/b = .06
  EXPECT_EQ(vector<string>({"a/a/a/a/a", "b/b/b/b"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  sorter.add("c/c/c");
  sorter.activate("c/c/c");
  sorter.allocated("c/c/c", slaveId, cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1").get();
  sorter.add("d/d");
  sorter.activate("d/d");
  sorter.allocated("d/d", slaveId, dResources);

  // shares: a/a/a/a/a = .05, b/b/b/b = .06, c/c/c = .01, d/d = .03
  EXPECT_EQ(vector<string>({"c/c/c", "d/d", "a/a/a/a/a", "b/b/b/b"}),
            sorter.sort());

  sorter.remove("a/a/a/a/a");

  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b/b/b/b", slaveId, bUnallocated);

  // shares: b/b/b/b = .02, c/c/c = .01, d/d = .03
  EXPECT_EQ(vector<string>({"c/c/c", "b/b/b/b", "d/d"}), sorter.sort());

  Resources eResources = Resources::parse("cpus:1;mem:5").get();
  sorter.add("e/e/e/e/e/e");
  sorter.activate("e/e/e/e/e/e");
  sorter.allocated("e/e/e/e/e/e", slaveId, eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(slaveId, removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b/b/b/b = .04, c/c/c = .02, d/d = .06, e/e/e/e/e/e = .05
  EXPECT_EQ(vector<string>({"c/c/c", "b/b/b/b", "e/e/e/e/e/e", "d/d"}),
            sorter.sort());

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(slaveId, addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1").get();
  sorter.add("f/f");
  sorter.activate("f/f");
  sorter.allocated("f/f", slaveId, fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15").get();
  sorter.allocated("c/c/c", slaveId, cResources2);

  // shares: b = .04, c = .08, d = .06, e = .025, f = .1
  EXPECT_EQ(vector<string>({"e/e/e/e/e/e", "b/b/b/b", "d/d", "c/c/c", "f/f"}),
            sorter.sort());

  EXPECT_TRUE(sorter.contains("b/b/b/b"));

  EXPECT_FALSE(sorter.contains("a/a/a/a/a"));

  EXPECT_EQ(5, sorter.count());

  sorter.deactivate("d/d");

  EXPECT_TRUE(sorter.contains("d/d"));

  EXPECT_EQ(vector<string>({"e/e/e/e/e/e", "b/b/b/b", "c/c/c", "f/f"}),
            sorter.sort());

  EXPECT_EQ(5, sorter.count());

  sorter.activate("d/d");

  EXPECT_EQ(vector<string>({"e/e/e/e/e/e", "b/b/b/b", "d/d", "c/c/c", "f/f"}),
            sorter.sort());
}


TEST(SorterTest, HierarchicalAllocation)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(slaveId, totalResources);

  sorter.add("a");
  sorter.add("b/c");
  sorter.add("b/d");
  sorter.activate("a");
  sorter.activate("b/c");
  sorter.activate("b/d");

  EXPECT_EQ(3, sorter.count());
  EXPECT_TRUE(sorter.contains("a"));
  EXPECT_FALSE(sorter.contains("b"));
  EXPECT_TRUE(sorter.contains("b/c"));
  EXPECT_TRUE(sorter.contains("b/d"));

  // Shares: a = 0, b/c = 0, b/d = 0.
  EXPECT_EQ(vector<string>({"a", "b/c", "b/d"}), sorter.sort());

  Resources aResources = Resources::parse("cpus:6;mem:6").get();
  sorter.allocated("a", slaveId, aResources);

  // Shares: a = 0.06, b/c = 0, b/d = 0.
  EXPECT_EQ(vector<string>({"b/c", "b/d", "a"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:4;mem:4").get();
  sorter.allocated("b/c", slaveId, cResources);

  Resources dResources = Resources::parse("cpus:3;mem:3").get();
  sorter.allocated("b/d", slaveId, dResources);

  // Shares: a = 0.06, b/d = 0.03, d = 0.04.
  EXPECT_EQ(vector<string>({"a", "b/d", "b/c"}), sorter.sort());

  {
    hashmap<string, Resources> agentAllocation =
      sorter.allocation(slaveId);

    EXPECT_EQ(3, agentAllocation.size());
    EXPECT_EQ(aResources, agentAllocation.at("a"));
    EXPECT_EQ(cResources, agentAllocation.at("b/c"));
    EXPECT_EQ(dResources, agentAllocation.at("b/d"));

    EXPECT_EQ(aResources, sorter.allocation("a", slaveId));
    EXPECT_EQ(cResources, sorter.allocation("b/c", slaveId));
    EXPECT_EQ(dResources, sorter.allocation("b/d", slaveId));
  }

  Resources aExtraResources = Resources::parse("cpus:2;mem:2").get();
  sorter.allocated("a", slaveId, aExtraResources);

  // Shares: b/d = 0.03, b/c = 0.04, a = 0.08.
  EXPECT_EQ(vector<string>({"b/d", "b/c", "a"}), sorter.sort());

  sorter.add("b/e/f");
  sorter.activate("b/e/f");

  EXPECT_FALSE(sorter.contains("b/e"));
  EXPECT_TRUE(sorter.contains("b/e/f"));

  // Shares: b/e/f = 0, b/d = 0.03, b/c = 0.04, a = 0.08.
  EXPECT_EQ(vector<string>({"b/e/f", "b/d", "b/c", "a"}), sorter.sort());

  Resources fResources = Resources::parse("cpus:3.5;mem:3.5").get();
  sorter.allocated("b/e/f", slaveId, fResources);

  // Shares: a = 0.08, b/d = 0.03, b/e/f = 0.035, b/c = 0.04.
  EXPECT_EQ(vector<string>({"a", "b/d", "b/e/f", "b/c"}), sorter.sort());

  // Removing a client should result in updating the fair-share for
  // the subtree that contains the removed client.
  sorter.remove("b/e/f");

  EXPECT_FALSE(sorter.contains("b/e/f"));
  EXPECT_EQ(3, sorter.count());

  // Shares: b/d = 0.03, b/c = 0.04, a = 0.08.
  EXPECT_EQ(vector<string>({"b/d", "b/c", "a"}), sorter.sort());

  // Updating a client should result in updating the fair-share for
  // the subtree that contains the updated client.
  Resources cNewResources = Resources::parse("cpus:1;mem:1").get();
  sorter.update("b/c", slaveId, cResources, cNewResources);

  // Shares: b/c = 0.01, b/d = 0.03, a = 0.08.
  EXPECT_EQ(vector<string>({"b/c", "b/d", "a"}), sorter.sort());

  sorter.add("b/e/f");
  sorter.activate("b/e/f");
  sorter.allocated("b/e/f", slaveId, fResources);

  // Shares: b/c = 0.01, b/d = 0.03, b/e/f = 0.035, a = 0.08.
  EXPECT_EQ(vector<string>({"b/c", "b/d", "b/e/f", "a"}), sorter.sort());

  EXPECT_EQ(4, sorter.count());
}


// This test checks that the sorted list of clients returned by the
// sorter iterates over the client tree in the correct order.
TEST(SorterTest, HierarchicalIterationOrder)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(slaveId, totalResources);

  sorter.add("a/b");
  sorter.add("c");
  sorter.add("d");
  sorter.add("d/e");

  sorter.activate("a/b");
  sorter.activate("c");
  sorter.activate("d");
  sorter.activate("d/e");

  // Shares: a/b = 0, c = 0, d = 0, d/e = 0
  EXPECT_EQ(vector<string>({"a/b", "c", "d", "d/e"}), sorter.sort());

  Resources cResources = Resources::parse("cpus:8;mem:8").get();
  sorter.allocated("c", slaveId, cResources);

  // Shares: a/b = 0, d = 0, d/e = 0, c = 0.08.
  EXPECT_EQ(vector<string>({"a/b", "d", "d/e", "c"}), sorter.sort());

  Resources dResources = Resources::parse("cpus:3;mem:3").get();
  sorter.allocated("d", slaveId, dResources);

  Resources deResources = Resources::parse("cpus:2;mem:2").get();
  sorter.allocated("d/e", slaveId, deResources);

  // Shares: a/b = 0, d/e = 0.02, d = 0.03, c = 0.08.
  EXPECT_EQ(vector<string>({"a/b", "d/e", "d", "c"}), sorter.sort());

  Resources abResources = Resources::parse("cpus:6;mem:6").get();
  sorter.allocated("a/b", slaveId, abResources);

  // Shares: d/e = 0.02, d = 0.03, a/b = 0.06, c = 0.08.
  EXPECT_EQ(vector<string>({"d/e", "d", "a/b", "c"}), sorter.sort());
}


// This test checks what happens when a new sorter client is added as
// a child of what was previously a leaf node.
TEST(SorterTest, AddChildToLeaf)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");
  sorter.activate("a");
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:6;mem:6").get());

  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  // Add a new client "a/c". The "a" subtree should now compete against
  // the "b" subtree; within the "a" subtree, "a" should compete (as a
  // sibling) against "a/c".

  sorter.add("a/c");
  sorter.activate("a/c");
  sorter.allocated(
      "a/c", slaveId, Resources::parse("cpus:5;mem:5").get());

  EXPECT_EQ(vector<string>({"b", "a/c", "a"}), sorter.sort());

  // Remove the "a" client; the "a/c" client should remain. Note that
  // "a/c" now appears before "b" in the sort order, because the "a"
  // subtree is now farther below its fair-share than the "b" subtree.

  sorter.remove("a");

  EXPECT_FALSE(sorter.contains("a"));
  EXPECT_EQ(vector<string>({"a/c", "b"}), sorter.sort());

  // Re-add the "a" client with the same resources. The client order
  // should revert to its previous value.
  sorter.add("a");
  sorter.activate("a");
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10").get());

  EXPECT_TRUE(sorter.contains("a"));
  EXPECT_EQ(vector<string>({"b", "a/c", "a"}), sorter.sort());

  // Check that "a" is considered to have a weight of 1 when it
  // competes against "a/c".
  sorter.updateWeight("a/c", 0.2);

  EXPECT_EQ(vector<string>({"b", "a", "a/c"}), sorter.sort());

  // Changing the weight "a" should change how it competes against its
  // siblings (e.g., "b"), not its children (e.g., "a/c").
  sorter.updateWeight("a", 3);

  EXPECT_EQ(vector<string>({"a", "a/c", "b"}), sorter.sort());

  sorter.updateWeight("a/c", 1);

  EXPECT_EQ(vector<string>({"a/c", "a", "b"}), sorter.sort());
}


// This test checks what happens when a new sorter client is added as
// a child of what was previously an internal node.
TEST(SorterTest, AddChildToInternal)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("x/a");
  sorter.activate("x/a");
  sorter.allocated(
      "x/a", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("x/b");
  sorter.activate("x/b");
  sorter.allocated(
      "x/b", slaveId, Resources::parse("cpus:6;mem:6").get());

  EXPECT_EQ(vector<string>({"x/b", "x/a"}), sorter.sort());

  sorter.add("x");
  sorter.activate("x");
  sorter.allocated(
      "x", slaveId, Resources::parse("cpus:7;mem:7").get());

  EXPECT_EQ(vector<string>({"x/b", "x", "x/a"}), sorter.sort());

  sorter.add("z");
  sorter.activate("z");
  sorter.allocated(
      "z", slaveId, Resources::parse("cpus:20;mem:20").get());

  EXPECT_EQ(vector<string>({"z", "x/b", "x", "x/a"}), sorter.sort());

  sorter.remove("x");

  EXPECT_EQ(vector<string>({"x/b", "x/a", "z"}), sorter.sort());
}


// This test checks what happens when a new sorter client is added as
// a child of what was previously an inactive leaf node.
TEST(SorterTest, AddChildToInactiveLeaf)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");
  sorter.activate("a");
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:6;mem:6").get());

  sorter.deactivate("a");

  EXPECT_EQ(vector<string>({"b"}), sorter.sort());

  sorter.add("a/c");
  sorter.activate("a/c");
  sorter.allocated(
      "a/c", slaveId, Resources::parse("cpus:5;mem:5").get());

  EXPECT_EQ(vector<string>({"b", "a/c"}), sorter.sort());
}


// This test checks what happens when a sorter client is removed,
// which allows a leaf node to be collapsed into its parent node. This
// is basically the inverse situation to `AddChildToLeaf`.
TEST(SorterTest, RemoveLeafCollapseParent)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");
  sorter.activate("a");
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:6;mem:6").get());

  sorter.add("a/c");
  sorter.activate("a/c");
  sorter.allocated(
      "a/c", slaveId, Resources::parse("cpus:5;mem:5").get());

  EXPECT_EQ(vector<string>({"b", "a/c", "a"}), sorter.sort());

  sorter.remove("a/c");

  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());
}


// This test checks what happens when a sorter client is removed and a
// leaf node can be collapsed into its parent node, we correctly
// propagate the `inactive` flag from leaf -> parent.
TEST(SorterTest, RemoveLeafCollapseParentInactive)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");
  sorter.activate("a");
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:6;mem:6").get());

  sorter.add("a/c");
  sorter.activate("a/c");
  sorter.allocated(
      "a/c", slaveId, Resources::parse("cpus:5;mem:5").get());

  sorter.deactivate("a");

  EXPECT_EQ(vector<string>({"b", "a/c"}), sorter.sort());

  sorter.remove("a/c");

  EXPECT_EQ(vector<string>({"b"}), sorter.sort());
}


// This test checks that setting a weight on an internal node works
// correctly.
TEST(SorterTest, ChangeWeightOnSubtree)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:100;mem:100").get());

  sorter.updateWeight("b", 3);
  sorter.updateWeight("a", 2);

  sorter.add("a/x");
  sorter.add("b/y");
  sorter.activate("a/x");
  sorter.activate("b/y");

  EXPECT_EQ(vector<string>({"a/x", "b/y"}), sorter.sort());

  sorter.allocated(
      "a/x", slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.allocated(
      "b/y", slaveId, Resources::parse("cpus:10;mem:10").get());

  EXPECT_EQ(vector<string>({"b/y", "a/x"}), sorter.sort());

  sorter.add("b/z");
  sorter.activate("b/z");
  sorter.allocated(
      "b/z", slaveId, Resources::parse("cpus:5;mem:5").get());

  EXPECT_EQ(vector<string>({"b/z", "b/y", "a/x"}), sorter.sort());

  sorter.add("b");
  sorter.activate("b");
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:4;mem:4").get());

  EXPECT_EQ(vector<string>({"a/x", "b", "b/z", "b/y"}), sorter.sort());

  sorter.add("a/zz");
  sorter.activate("a/zz");
  sorter.allocated(
      "a/zz", slaveId, Resources::parse("cpus:2;mem:2").get());

  EXPECT_EQ(vector<string>({"a/zz", "a/x", "b", "b/z", "b/y"}), sorter.sort());
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
  sorter.activate("a");
  sorter.activate("b");

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

  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
}


TEST(SorterTest, UpdateAllocation)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a");
  sorter.add("b");
  sorter.activate("a");
  sorter.activate("b");

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
  EXPECT_EQ(newAllocation.get(), allocation.at(slaveId));
  EXPECT_EQ(newAllocation.get(), sorter.allocation("a", slaveId));
}


TEST(SorterTest, UpdateAllocationNestedClient)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add("a/x");
  sorter.add("b/y");
  sorter.activate("a/x");
  sorter.activate("b/y");

  sorter.add(slaveId, Resources::parse("cpus:10;mem:10;disk:10").get());

  sorter.allocated(
      "a/x", slaveId, Resources::parse("cpus:10;mem:10;disk:10").get());

  // Construct an offer operation.
  Resource volume = Resources::parse("disk", "5", "*").get();
  volume.mutable_disk()->mutable_persistence()->set_id("ID");
  volume.mutable_disk()->mutable_volume()->set_container_path("data");

  // Compute the updated allocation.
  Resources oldAllocation = sorter.allocation("a/x", slaveId);
  Try<Resources> newAllocation = oldAllocation.apply(CREATE(volume));
  ASSERT_SOME(newAllocation);

  // Update the resources for the client.
  sorter.update("a/x", slaveId, oldAllocation, newAllocation.get());

  hashmap<SlaveID, Resources> allocation = sorter.allocation("a/x");
  EXPECT_EQ(1u, allocation.size());
  EXPECT_EQ(newAllocation.get(), allocation.at(slaveId));
  EXPECT_EQ(newAllocation.get(), sorter.allocation("a/x", slaveId));
}


// This test checks that the sorter correctly reports allocation
// information about inactive clients.
TEST(SorterTest, AllocationForInactiveClient)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  sorter.add(slaveId, Resources::parse("cpus:10;mem:10").get());

  sorter.add("a");
  sorter.add("b");

  // Leave client "a" inactive.
  sorter.activate("b");

  sorter.allocated("a", slaveId, Resources::parse("cpus:2;mem:2").get());
  sorter.allocated("b", slaveId, Resources::parse("cpus:3;mem:3").get());

  hashmap<string, Resources> clientAllocation = sorter.allocation(slaveId);
  EXPECT_EQ(2u, clientAllocation.size());
  EXPECT_EQ(Resources::parse("cpus:2;mem:2").get(), clientAllocation.at("a"));
  EXPECT_EQ(Resources::parse("cpus:3;mem:3").get(), clientAllocation.at("b"));

  hashmap<SlaveID, Resources> agentAllocation1 = sorter.allocation("a");
  EXPECT_EQ(1u, agentAllocation1.size());
  EXPECT_EQ(
      Resources::parse("cpus:2;mem:2").get(), agentAllocation1.at(slaveId));

  hashmap<SlaveID, Resources> agentAllocation2 = sorter.allocation("b");
  EXPECT_EQ(1u, agentAllocation2.size());
  EXPECT_EQ(
      Resources::parse("cpus:3;mem:3").get(), agentAllocation2.at(slaveId));
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
  sorter.activate("framework");

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
  sorter.activate("framework");

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
  sorter.activate("a");
  sorter.activate("b");

  sorter.add(slaveId, Resources::parse("cpus:10;mem:100").get());

  // Dominant share of "a" is 0.2 (cpus).
  sorter.allocated(
      "a", slaveId, Resources::parse("cpus:2;mem:1").get());

  // Dominant share of "b" is 0.1 (cpus).
  sorter.allocated(
      "b", slaveId, Resources::parse("cpus:1;mem:2").get());

  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  // Update the total resources by removing the previous total and
  // adding back the new total.
  sorter.remove(slaveId, Resources::parse("cpus:10;mem:100").get());
  sorter.add(slaveId, Resources::parse("cpus:100;mem:10").get());

  // Now the dominant share of "a" is 0.1 (mem) and "b" is 0.2 (mem),
  // which should change the sort order.
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
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
  sorter.activate("a");
  sorter.activate("b");

  sorter.add(slaveA, Resources::parse("cpus:5;mem:50").get());
  sorter.add(slaveB, Resources::parse("cpus:5;mem:50").get());

  // Dominant share of "a" is 0.2 (cpus).
  sorter.allocated(
      "a", slaveA, Resources::parse("cpus:2;mem:1").get());

  // Dominant share of "b" is 0.1 (cpus).
  sorter.allocated(
      "b", slaveB, Resources::parse("cpus:1;mem:3").get());

  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  // Update the total resources of slaveA by removing the previous
  // total and adding the new total.
  sorter.remove(slaveA, Resources::parse("cpus:5;mem:50").get());
  sorter.add(slaveA, Resources::parse("cpus:95;mem:50").get());

  // Now the dominant share of "a" is 0.02 (cpus) and "b" is 0.03
  // (mem), which should change the sort order.
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
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
  sorter.activate("a");
  sorter.activate("b");

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
  EXPECT_EQ(a, sorter.allocation("a", slaveId));
  EXPECT_EQ(b, sorter.allocation("b", slaveId));

  // Check that the sort is correct.
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
}


// This test verifies that shared resources are properly accounted for in
// the DRF sorter.
TEST(SorterTest, SharedResources)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resource sharedDisk = createDiskResource(
      "100", "role1", "id1", "path1", None(), true);

  // Set totalResources to have disk of 1000, with disk 100 being shared.
  Resources totalResources = Resources::parse(
      "cpus:100;mem:100;disk(role1):900").get();
  totalResources += sharedDisk;

  sorter.add(slaveId, totalResources);

  // Verify sort() works when shared resources are in the allocations.
  sorter.add("a");
  sorter.activate("a");
  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  aResources += sharedDisk;
  sorter.allocated("a", slaveId, aResources);

  sorter.add("b");
  sorter.activate("b");
  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.allocated("b", slaveId, bResources);

  // Shares: a = .1 (dominant: disk), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  sorter.add("c");
  sorter.activate("c");
  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  cResources += sharedDisk;
  sorter.allocated("c", slaveId, cResources);

  // 'a' and 'c' share the same persistent volume which is the
  // dominant resource for both of these clients.
  // Shares: a = .1 (dominant: disk), b = .06 (dominant: cpus),
  //         c = .1 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "a", "c"}), sorter.sort());

  sorter.remove("a");
  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b", slaveId, bUnallocated);

  // Shares: b = .02 (dominant: cpus), c = .1 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "c"}), sorter.sort());

  sorter.add("d");
  sorter.activate("d");
  Resources dResources = Resources::parse("cpus:1;mem:5").get();
  dResources += sharedDisk;
  sorter.allocated("d", slaveId, dResources);

  // Shares: b = .02 (dominant: cpus), c = .1 (dominant: disk),
  //         d = .1 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "c", "d"}), sorter.sort());

  // Verify other basic allocator methods work when shared resources
  // are in the allocations.
  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(slaveId, removedResources);

  // Total resources is now:
  // cpus:50;mem:100;disk(role1):900;disk(role1)[id1]:100

  // Shares: b = .04 (dominant: cpus), c = .1 (dominant: disk),
  //         d = .1 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "c", "d"}), sorter.sort());

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(slaveId, addedResources);

  // Total resources is now:
  // cpus:50;mem:200;disk(role1):900;disk(role1)[id1]:100

  // Shares: b = .04 (dominant: cpus), c = .1 (dominant: disk),
  //         d = .1 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "c", "d"}), sorter.sort());

  EXPECT_TRUE(sorter.contains("b"));

  EXPECT_FALSE(sorter.contains("a"));

  EXPECT_EQ(3, sorter.count());
}


// This test verifies that shared resources can make clients
// indistinguishable with its high likelihood of becoming the
// dominant resource.
TEST(SorterTest, SameDominantSharedResourcesAcrossClients)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resource sharedDisk = createDiskResource(
      "900", "role1", "id1", "path1", None(), true);

  // Set totalResources to have disk of 1000, with disk 900 being shared.
  Resources totalResources = Resources::parse(
      "cpus:100;mem:100;disk(role1):100").get();
  totalResources += sharedDisk;

  sorter.add(slaveId, totalResources);

  // Add 2 clients each with the same shared disk, but with varying
  // cpus and mem.
  sorter.add("b");
  sorter.activate("b");
  Resources bResources = Resources::parse("cpus:5;mem:20").get();
  bResources += sharedDisk;
  sorter.allocated("b", slaveId, bResources);

  sorter.add("c");
  sorter.activate("c");
  Resources cResources = Resources::parse("cpus:10;mem:6").get();
  cResources += sharedDisk;
  sorter.allocated("c", slaveId, cResources);

  // Shares: b = .9 (dominant: disk), c = .9 (dominant: disk).
  EXPECT_EQ(vector<string>({"b", "c"}), sorter.sort());

  // Add 3rd client with the same shared resource.
  sorter.add("a");
  sorter.activate("a");
  Resources aResources = Resources::parse("cpus:50;mem:40").get();
  aResources += sharedDisk;
  sorter.allocated("a", slaveId, aResources);

  // Shares: a = .9 (dominant: disk), b = .9 (dominant: disk),
  //         c = .9 (dominant: disk).
  EXPECT_EQ(vector<string>({"a", "b", "c"}), sorter.sort());
}


// This test verifies that allocating the same shared resource to the
// same client does not alter its fair share.
TEST(SorterTest, SameSharedResourcesSameClient)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resource sharedDisk = createDiskResource(
      "50", "role1", "id1", "path1", None(), true);

  // Set totalResources to have disk of 1000, with disk of 50 being shared.
  Resources totalResources = Resources::parse(
      "cpus:100;mem:100;disk(role1):950").get();
  totalResources += sharedDisk;

  sorter.add(slaveId, totalResources);

  // Verify sort() works when shared resources are in the allocations.
  sorter.add("a");
  sorter.activate("a");
  Resources aResources = Resources::parse("cpus:2;mem:2").get();
  aResources += sharedDisk;
  sorter.allocated("a", slaveId, aResources);

  sorter.add("b");
  sorter.activate("b");
  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.allocated("b", slaveId, bResources);

  // Shares: a = .05 (dominant: disk), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());

  // Update a's share to allocate 3 more copies of the shared disk.
  // Verify fair share does not change when additional copies of same
  // shared resource are added to a specific client.
  Resources additionalAShared = Resources(sharedDisk) + sharedDisk + sharedDisk;
  sorter.allocated("a", slaveId, additionalAShared);

  // Shares: a = .05 (dominant: disk), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
}


// This test verifies that shared resources are unallocated when all
// the copies are unallocated.
TEST(SorterTest, SharedResourcesUnallocated)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resource sharedDisk = createDiskResource(
      "100", "role1", "id1", "path1", None(), true);

  // Set totalResources to have disk of 1000, with disk 100 being shared.
  Resources totalResources = Resources::parse(
      "cpus:100;mem:100;disk(role1):900").get();
  totalResources += sharedDisk;

  sorter.add(slaveId, totalResources);

  // Allocate 3 copies of shared resources to client 'a', but allocate no
  // shared resource to client 'b'.
  sorter.add("a");
  sorter.activate("a");
  Resources aResources = Resources::parse("cpus:2;mem:2").get();
  aResources += sharedDisk;
  aResources += sharedDisk;
  aResources += sharedDisk;
  sorter.allocated("a", slaveId, aResources);

  sorter.add("b");
  sorter.activate("b");
  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.allocated("b", slaveId, bResources);

  // Shares: a = .1 (dominant: disk), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  // Unallocate 1 copy of shared resource from client 'a', which should
  // result in no change in its dominant share.
  sorter.unallocated("a", slaveId, sharedDisk);

  // Shares: a = .1 (dominant: disk), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"b", "a"}), sorter.sort());

  // Unallocate remaining copies of shared resource from client 'a',
  // which would affect the fair share.
  sorter.unallocated("a", slaveId, Resources(sharedDisk) + sharedDisk);

  // Shares: a = .02 (dominant: cpus), b = .06 (dominant: cpus).
  EXPECT_EQ(vector<string>({"a", "b"}), sorter.sort());
}


// This test verifies that shared resources are removed from the sorter
// only when all instances of the the same shared resource are removed.
TEST(SorterTest, RemoveSharedResources)
{
  DRFSorter sorter;

  SlaveID slaveId;
  slaveId.set_value("agentId");

  Resource sharedDisk = createDiskResource(
      "100", "role1", "id1", "path1", None(), true);

  sorter.add(
      slaveId, Resources::parse("cpus:100;mem:100;disk(role1):900").get());

  Resources quantity1 = sorter.totalScalarQuantities();

  sorter.add(slaveId, sharedDisk);
  Resources quantity2 = sorter.totalScalarQuantities();

  EXPECT_EQ(Resources::parse("disk(role1):100").get(), quantity2 - quantity1);

  sorter.add(slaveId, sharedDisk);
  Resources quantity3 = sorter.totalScalarQuantities();

  EXPECT_NE(quantity1, quantity3);
  EXPECT_EQ(quantity2, quantity3);

  // The quantity of the shared disk is removed  when the last copy is removed.
  sorter.remove(slaveId, sharedDisk);
  EXPECT_EQ(sorter.totalScalarQuantities(), quantity3);

  sorter.remove(slaveId, sharedDisk);
  EXPECT_EQ(sorter.totalScalarQuantities(), quantity1);
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

  // TODO(gyliu513): Parameterize the number of range for the fragment.
  Try<::mesos::Value::Ranges> ranges = fragment(createRange(31000, 32000), 100);
  ASSERT_SOME(ranges);
  ASSERT_EQ(100, ranges->range_size());

  allocated += createPorts(ranges.get());

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

  watch.start();
  {
    // Unallocate resources on all agents, round-robin through the clients.
    size_t clientIndex = 0;
    foreach (const SlaveID& slaveId, agents) {
      const string& client = clients[clientIndex++ % clients.size()];
      sorter.unallocated(client, slaveId, allocated);
    }
  }
  watch.stop();

  cout << "Removed allocations for " << agentCount << " agents in "
         << watch.elapsed() << endl;

  watch.start();
  {
    foreach (const SlaveID& slaveId, agents) {
      sorter.remove(slaveId, agentResources);
    }
  }
  watch.stop();

  cout << "Removed " << agentCount << " agents in "
       << watch.elapsed() << endl;

  watch.start();
  {
    foreach (const string& clientId, clients) {
      sorter.remove(clientId);
    }
  }
  watch.stop();

  cout << "Removed " << clientCount << " clients in "
       << watch.elapsed() << endl;
}


class HierarchicalSorter_BENCHMARK_Test
  : public ::testing::Test,
    public ::testing::WithParamInterface<
        std::tr1::tuple<size_t, std::tr1::tuple<size_t, size_t>>> {};


INSTANTIATE_TEST_CASE_P(
    AgentAndClientCount,
    HierarchicalSorter_BENCHMARK_Test,
    ::testing::Combine(
      ::testing::Values(1000U, 5000U, 10000U, 20000U, 30000U, 50000U),
      ::testing::Values(
          // ~1000 clients with different heights and branching factors.
          std::tr1::tuple<size_t, size_t>{3U, 32U},   // 1056 clients.
          std::tr1::tuple<size_t, size_t>{7U, 3U},    // 1092 clients.
          std::tr1::tuple<size_t, size_t>{10U, 2U}))  // 1022 clients.
    );


// This benchmark simulates sorting a hierarchy of clients that have
// different amount of allocations. The shape of the hierarchy is
// determined by two parameters: height (depth of the hierarchy
// including the root node) and branching factor (number of children
// of each internal node).
TEST_P(HierarchicalSorter_BENCHMARK_Test, FullSort)
{
  const size_t agentCount = std::tr1::get<0>(GetParam());
  const std::tr1::tuple<size_t, size_t> tuple = std::tr1::get<1>(GetParam());
  const size_t treeHeight = std::tr1::get<0>(tuple);
  const size_t branchingFactor = std::tr1::get<1>(tuple);

  vector<SlaveID> agents;
  agents.reserve(agentCount);

  // Compute total number of clients in a tree of given depth and
  // breadth, including root node.
  std::function<size_t (size_t)> totalClients =
    [&totalClients, branchingFactor](size_t depth) -> size_t {
      if (depth == 0 || depth == 1) {
        return depth;
      }

      return 1 + branchingFactor * totalClients(depth - 1);
    };

  const size_t clientCount = totalClients(treeHeight) - 1;

  vector<string> clients;
  clients.reserve(clientCount);

  DRFSorter sorter;
  Stopwatch watch;

  watch.start();
  {
    // Build a tree of given depth and branching factor in depth-first fashion.
    std::function<void (string, size_t)> buildTree =
      [&buildTree, &sorter, &clients, branchingFactor](
          string path, size_t depth) {
        if (depth == 0) {
          return;
        }

        for (size_t i = 0; i < branchingFactor; i++) {
          buildTree(path + stringify(i) + "/", depth - 1);
        }

        const string client = strings::remove(path, "/", strings::SUFFIX);
        if (!client.empty()) {
          sorter.add(client);
          clients.push_back(client);
        }
      };

    buildTree("", treeHeight);
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

  Resources allocated = Resources::parse("cpus:16;mem:2014;disk:1024").get();

  // TODO(gyliu513): Parameterize the number of range for the fragment.
  Try<::mesos::Value::Ranges> ranges = fragment(createRange(31000, 32000), 100);
  ASSERT_SOME(ranges);
  ASSERT_EQ(100, ranges->range_size());

  allocated += createPorts(ranges.get());

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

  watch.start();
  {
    // Unallocate resources on all agents, round-robin through the clients.
    size_t clientIndex = 0;
    foreach (const SlaveID& slaveId, agents) {
      const string& client = clients[clientIndex++ % clients.size()];
      sorter.unallocated(client, slaveId, allocated);
    }
  }
  watch.stop();

  cout << "Removed allocations for " << agentCount << " agents in "
         << watch.elapsed() << endl;

  watch.start();
  {
    foreach (const SlaveID& slaveId, agents) {
      sorter.remove(slaveId, agentResources);
    }
  }
  watch.stop();

  cout << "Removed " << agentCount << " agents in "
       << watch.elapsed() << endl;

  watch.start();
  {
    foreach (const string& clientId, clients) {
      sorter.remove(clientId);
    }
  }
  watch.stop();

  cout << "Removed " << clientCount << " clients in "
       << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
