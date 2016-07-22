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

#include <atomic>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/allocator/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/utils.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::MIN_CPUS;
using mesos::internal::master::MIN_MEM;

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::protobuf::createLabel;

using mesos::allocator::Allocator;

using process::Clock;
using process::Future;

using std::atomic;
using std::cout;
using std::endl;
using std::map;
using std::set;
using std::string;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {


struct Allocation
{
  FrameworkID frameworkId;
  hashmap<SlaveID, Resources> resources;
};


// Creates a "ports(*)" resource for the given ranges.
static Resource createPorts(const ::mesos::Value::Ranges& ranges)
{
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


// Fragments the given range bounds into a number of subranges.
// Returns an Error if 'numRanges' is too large. E.g.
//
//   [1-10], 1 -> [1-10]
//   [1-10], 2 -> [1-1,3-10]
//   [1-10], 3 -> [1-1,3-3,5-10]
//   [1-10], 4 -> [1-1,3-3,5-5,7-10]
//   [1-10], 5 -> [1-1,3-3,5-5,7-7,9-10]
//   [1-10], 6 -> Error
//
static Try<::mesos::Value::Ranges> fragment(
    const ::mesos::Value::Range& bounds,
    size_t numRanges)
{
  uint64_t numValues = bounds.end() - bounds.begin() + 1;

  // Compute the max number of ranges.
  //
  // If `numValues` is even, then the maximum number of ranges is
  // `numValues / 2`:
  //   [1-2] -> 2 values, maximum 1 range:  [1-2]
  //   [1-4] -> 4 values, maximum 2 ranges: [1-1,3-4]
  //   [1-6] -> 6 values, maximum 3 ranges: [1-1,3-3,5-6]
  //
  // If `numValues` is odd, then the maximum number of ranges is
  // `(numValues + 1) / 2`:
  //   [1-1] -> 1 values, maximum 1 range:  [1-1]
  //   [1-3] -> 3 values, maximum 2 ranges: [1-1,3-3]
  //   [1-5] -> 5 values, maximum 3 ranges: [1-1,3-3,5-5]
  //
  uint64_t maxRanges;
  if (numValues % 2 == 0) {
    maxRanges = numValues / 2;
  } else {
    maxRanges = (numValues + 1) / 2;
  }

  if (numRanges > maxRanges) {
    return Error("Requested more distinct ranges than possible");
  }

  // See the documentation above for the fragmentation technique.
  // We fragment from the front of the bounds until we have the
  // desired number of ranges.
  ::mesos::Value::Ranges ranges;
  ranges.mutable_range()->Reserve(numRanges);

  for (size_t i = 0; i < numRanges; ++i) {
    Value::Range* range = ranges.add_range();

    range->set_begin(bounds.begin() + (i * 2));
    range->set_end(range->begin());
  }

  // Make sure the last range covers the end of the bounds.
  if (!ranges.range().empty()) {
    ranges.mutable_range()->rbegin()->set_end(bounds.end());
  }

  return ranges;
}


static ::mesos::Value::Range createRange(uint64_t begin, uint64_t end)
{
  ::mesos::Value::Range range;
  range.set_begin(begin);
  range.set_end(end);
  return range;
}


struct Deallocation
{
  FrameworkID frameworkId;
  hashmap<SlaveID, UnavailableResources> resources;
};


class HierarchicalAllocatorTestBase : public ::testing::Test
{
protected:
  HierarchicalAllocatorTestBase()
    : allocator(createAllocator<HierarchicalDRFAllocator>()),
      nextSlaveId(1),
      nextFrameworkId(1) {}

  ~HierarchicalAllocatorTestBase()
  {
    delete allocator;
  }

  void initialize(
      const master::Flags& _flags = master::Flags(),
      Option<lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>> offerCallback = None(),
      Option<lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>>
                 inverseOfferCallback = None())
  {
    flags = _flags;

    if (offerCallback.isNone()) {
      offerCallback =
        [this](const FrameworkID& frameworkId,
               const hashmap<SlaveID, Resources>& resources) {
          Allocation allocation;
          allocation.frameworkId = frameworkId;
          allocation.resources = resources;

          allocations.put(allocation);
        };
    }

    if (inverseOfferCallback.isNone()) {
      inverseOfferCallback =
        [this](const FrameworkID& frameworkId,
               const hashmap<SlaveID, UnavailableResources>& resources) {
          Deallocation deallocation;
          deallocation.frameworkId = frameworkId;
          deallocation.resources = resources;

          deallocations.put(deallocation);
        };
    }

    allocator->initialize(
        flags.allocation_interval,
        offerCallback.get(),
        inverseOfferCallback.get(),
        {},
        flags.fair_sharing_excluded_resource_names);
  }

  SlaveInfo createSlaveInfo(const string& resources)
  {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(nextSlaveId++));

    SlaveInfo slave;
    *(slave.mutable_resources()) = Resources::parse(resources).get();
    *(slave.mutable_id()) = slaveId;
    slave.set_hostname(slaveId.value());

    return slave;
  }

  FrameworkInfo createFrameworkInfo(
      const string& role,
      const vector<FrameworkInfo::Capability::Type>& capabilities = {})
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("framework" + stringify(nextFrameworkId++));
    frameworkInfo.mutable_id()->set_value(frameworkInfo.name());
    frameworkInfo.set_role(role);

    foreach (const FrameworkInfo::Capability::Type& capability, capabilities) {
      frameworkInfo.add_capabilities()->set_type(capability);
    }

    return frameworkInfo;
  }

  static Quota createQuota(const string& role, const string& resources)
  {
    mesos::quota::QuotaInfo quotaInfo;
    quotaInfo.set_role(role);
    quotaInfo.mutable_guarantee()->CopyFrom(Resources::parse(resources).get());

    return Quota{quotaInfo};
  }

  Resources createRevocableResources(
      const string& name,
      const string& value,
      const string& role = "*")
  {
    Resource resource = Resources::parse(name, value, role).get();
    resource.mutable_revocable();
    return resource;
  }

  static WeightInfo createWeightInfo(const string& role, double weight)
  {
    WeightInfo weightInfo;
    weightInfo.set_role(role);
    weightInfo.set_weight(weight);

    return weightInfo;
  }

protected:
  master::Flags flags;

  Allocator* allocator;

  process::Queue<Allocation> allocations;
  process::Queue<Deallocation> deallocations;

private:
  int nextSlaveId;
  int nextFrameworkId;
};


class HierarchicalAllocatorTest : public HierarchicalAllocatorTestBase {};


// TODO(bmahler): These tests were transformed directly from
// integration tests into unit tests. However, these tests
// should be simplified even further to each test a single
// expected behavior, at which point we can have more tests
// that are each very small.


// Checks that the DRF allocator implements the DRF algorithm
// correctly. The test accomplishes this by adding frameworks and
// slaves one at a time to the allocator, making sure that each time
// a new slave is added all of its resources are offered to whichever
// framework currently has the smallest share. Checking for proper DRF
// logic when resources are returned, frameworks exit, etc. is handled
// by SorterTest.DRFSorter.
TEST_F(HierarchicalAllocatorTest, UnreservedDRF)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Total cluster resources will become cpus=2, mem=1024.
  SlaveInfo slave1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), {});

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 1

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  // Total cluster resources will become cpus=3, mem=1536:
  // role1 share = 0.66 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0
  SlaveInfo slave2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), {});

  // framework2 will be offered all of slave2's resources since role2
  // has the lowest user share, and framework2 is its only framework.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave2.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 0.67 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0.33 (cpus=1, mem=512)
  //   framework2 share = 1

  // Total cluster resources will become cpus=6, mem=3584:
  // role1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0.16 (cpus=1, mem=512)
  //   framework2 share = 1
  SlaveInfo slave3 = createSlaveInfo("cpus:3;mem:2048;disk:0");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), {});

  // framework2 will be offered all of slave3's resources since role2
  // has the lowest share.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0.71 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(framework3.id(), framework3, {});

  // Total cluster resources will become cpus=10, mem=7680:
  // role1 share = 0.2 (cpus=2, mem=1024)
  //   framework1 share = 1
  //   framework3 share = 0
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave4 = createSlaveInfo("cpus:4;mem:4096;disk:0");
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), {});

  // framework3 will be offered all of slave4's resources since role1
  // has the lowest user share, and framework3 has the lowest share of
  // role1's frameworks.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 0.67 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework4 = createFrameworkInfo("role1");
  allocator->addFramework(framework4.id(), framework4, {});

  // Total cluster resources will become cpus=11, mem=8192
  // role1 share = 0.63 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  //   framework4 share = 0
  // role2 share = 0.36 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave5 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave5.id(), slave5, None(), slave5.resources(), {});

  // Even though framework4 doesn't have any resources, role2 has a
  // lower share than role1, so framework2 receives slave5's resources.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave5.resources(), Resources::sum(allocation.get().resources));
}


// This test ensures that reserved resources do affect the sharing across roles.
TEST_F(HierarchicalAllocatorTest, ReservedDRF)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  SlaveInfo slave1 = createSlaveInfo(
      "cpus:1;mem:512;disk:0;"
      "cpus(role1):100;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), {});

  // framework1 will be offered all of the resources.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  // framework2 will be allocated the new resources.
  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave2.resources(), Resources::sum(allocation.get().resources));

  // Since `framework1` has more resources allocated to it than `framework2`,
  // We expect `framework2` to receive this agent's resources.
  SlaveInfo slave3 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // Now add another framework in role1. Since the reserved resources
  // should be allocated fairly between frameworks within a role, we
  // expect framework3 to receive the next allocation of role1
  // resources.
  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(framework3.id(), framework3, {});

  SlaveInfo slave4 = createSlaveInfo(
      "cpus(role1):2;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));
}


// Tests that the fairness exclusion list works as expected. The test
// accomplishes this by adding frameworks and slaves one at a time to
// the allocator with exclude resources, making sure that each time a
// new slave is added all of its resources are offered to whichever
// framework currently has the smallest share. Checking for proper DRF
// logic when resources are returned, frameworks exit, etc, is handled
// by SorterTest.DRFSorter.
TEST_F(HierarchicalAllocatorTest, DRFWithFairnessExclusion)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  // Specify that `gpus` should not be fairly shared.
  master::Flags flags_;
  flags_.fair_sharing_excluded_resource_names = set<string>({"gpus"});

  initialize(flags_);

  // Total cluster resources will become cpus=2, mem=1024, gpus=1.
  SlaveInfo agent1 = createSlaveInfo("cpus:2;mem:1024;disk:0;gpus:1");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  // framework1 will be offered all of agent1's resources since it is
  // the only framework running so far.
  FrameworkInfo framework1 = createFrameworkInfo(
      "role1", {FrameworkInfo::Capability::GPU_RESOURCES});

  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation->frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation->resources));

  // role1 share = 1 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework1 share = 1

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  // Total cluster resources will become cpus=3, mem=1536, (ignored) gpus=1
  // role1 share = 0.66 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // framework2 will be offered all of agent2's resources since role2
  // has the lowest user share, and framework2 is its only framework.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation->frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation->resources));

  // role1 share = 0.67 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework1 share = 1
  // role2 share = 0.33 (cpus=1, mem=512)
  //   framework2 share = 1

  // Total cluster resources will become cpus=6, mem=3584, (ignored) gpus=1
  // role1 share = 0.33 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework1 share = 1
  // role2 share = 0.16 (cpus=1, mem=512)
  //   framework2 share = 1
  SlaveInfo agent3 = createSlaveInfo("cpus:3;mem:2048;disk:0");
  allocator->addSlave(agent3.id(), agent3, None(), agent3.resources(), {});

  // framework2 will be offered all of agent3's resources since role2
  // has the lowest share.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation->frameworkId);
  EXPECT_EQ(agent3.resources(), Resources::sum(allocation->resources));

  // role1 share = 0.33 (cpus=2, mem=1024, (ignored)gpus=1)
  //   framework1 share = 1
  // role2 share = 0.71 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(framework3.id(), framework3, {});

  // Total cluster resources will become cpus=10, mem=7680, (ignored) gpus=1
  // role1 share = 0.2 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework1 share = 1
  //   framework3 share = 0
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo agent4 = createSlaveInfo("cpus:4;mem:4096;disk:0");
  allocator->addSlave(agent4.id(), agent4, None(), agent4.resources(), {});

  // framework3 will be offered all of agent4's resources since role1
  // has the lowest user share, and framework3 has the lowest share of
  // role1's frameworks.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation->frameworkId);
  EXPECT_EQ(agent4.resources(), Resources::sum(allocation->resources));

  // role1 share = 0.67 (cpus=6, mem=5120, (ignored) gpus=1)
  //   framework1 share = 0.33 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework4 = createFrameworkInfo("role1");
  allocator->addFramework(framework4.id(), framework4, {});

  // Total cluster resources will become cpus=11, mem=8192, (ignored) gpus=1
  // role1 share = 0.63 (cpus=6, mem=5120, (ignored) gpus=1)
  //   framework1 share = 0.33 (cpus=2, mem=1024, (ignored) gpus=1)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  //   framework4 share = 0
  // role2 share = 0.36 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo agent5 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent5.id(), agent5, None(), agent5.resources(), {});

  // Even though framework4 doesn't have any resources, role2 has a
  // lower share than role1, so framework2 receives agent5's resources.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation->frameworkId);
  EXPECT_EQ(agent5.resources(), Resources::sum(allocation->resources));
}


// This test ensures that an offer filter larger than the
// allocation interval effectively filters out resources.
TEST_F(HierarchicalAllocatorTest, OfferFilter)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  // We put both frameworks into the same role, but we could also
  // have had separate roles; this should not influence the test.
  const string ROLE{"role"};

  initialize();

  FrameworkInfo framework = createFrameworkInfo(ROLE);
  allocator->addFramework(framework.id(), framework, {});

  SlaveInfo agent = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // `framework` will be offered all of `agent` resources
  // because it is the only framework in the cluster.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  // Now `framework` declines the offer and sets a filter
  // with the duration greater than the allocation interval.
  Duration filterTimeout = flags.allocation_interval * 2;
  Filters offerFilter;
  offerFilter.set_refuse_seconds(filterTimeout.secs());

  allocator->recoverResources(
      framework.id(),
      agent.id(),
      allocation.get().resources.get(agent.id()).get(),
      offerFilter);

  // Ensure the offer filter timeout is set before advancing the clock.
  Clock::settle();

  JSON::Object metrics = Metrics();

  string activeOfferFilters =
    "allocator/mesos/offer_filters/roles/" + ROLE + "/active";
  EXPECT_EQ(1, metrics.values[activeOfferFilters]);

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // There should be no allocation due to the offer filter.
  allocation = allocations.get();
  ASSERT_TRUE(allocation.isPending());

  // Ensure the offer filter times out (2x the allocation interval)
  // and the next batch allocation occurs.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // The next batch allocation should offer resources to `framework1`.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  metrics = Metrics();

  EXPECT_EQ(0, metrics.values[activeOfferFilters]);
}


// This test ensures that an offer filter is not removed earlier than
// the next batch allocation. See MESOS-4302 for more information.
//
// NOTE: If we update the code to allocate upon resource recovery
// (MESOS-3078), this test should still pass in that the small offer
// filter timeout should lead to the next allocation for the agent
// applying the filter.
TEST_F(HierarchicalAllocatorTest, SmallOfferFilterTimeout)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  // We put both frameworks into the same role, but we could also
  // have had separate roles; this should not influence the test.
  const string ROLE{"role"};

  // Explicitly set the allocation interval to make sure
  // it is greater than the offer filter timeout.
  master::Flags flags_;
  flags_.allocation_interval = Minutes(1);

  initialize(flags_);

  FrameworkInfo framework1 = createFrameworkInfo(ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo(ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  // Total cluster resources (1 agent): cpus=1, mem=512.
  // ROLE1 share = 1 (cpus=1, mem=512)
  //   framework1 share = 1 (cpus=1, mem=512)
  //   framework2 share = 0

  // Add one more agent with some free resources.
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // Process the allocation triggered by the agent addition.
  Clock::settle();

  // `framework2` will be offered all of `agent2` resources
  // because its share (0) is smaller than `framework1`.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (2 agents): cpus=2, mem=1024.
  // ROLE1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 0.5 (cpus=1, mem=512)
  //   framework2 share = 0.5 (cpus=1, mem=512)

  // Now `framework2` declines the offer and sets a filter
  // for 1 second, which is less than the allocation interval.
  Duration filterTimeout = Seconds(1);
  ASSERT_GT(flags.allocation_interval, filterTimeout);

  Filters offerFilter;
  offerFilter.set_refuse_seconds(filterTimeout.secs());

  allocator->recoverResources(
      framework2.id(),
      agent2.id(),
      allocation.get().resources.get(agent2.id()).get(),
      offerFilter);

  // Total cluster resources (2 agents): cpus=2, mem=1024.
  // ROLE1 share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1 (cpus=1, mem=512)
  //   framework2 share = 0

  // The offer filter times out. Since the allocator ensures that
  // offer filters are removed after at least one batch allocation
  // has occurred, we expect that after the timeout elapses, the
  // filter will remain active for the next allocation and the
  // resources are allocated to `framework1`.
  Clock::advance(filterTimeout);
  Clock::settle();

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // Since the filter is applied, resources are offered to `framework1`
  // even though its share is greater than `framework2`.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (2 agents): cpus=2, mem=1024.
  // ROLE1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 1 (cpus=2, mem=1024)
  //   framework2 share = 0

  // The filter should be removed now than the batch
  // allocation has occurred!

  // Now `framework1` declines the offer.
  allocator->recoverResources(
      framework1.id(),
      agent2.id(),
      allocation.get().resources.get(agent2.id()).get(),
      None());

  // Total cluster resources (2 agents): cpus=2, mem=1024.
  // ROLE1 share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1 (cpus=1, mem=512)
  //   framework2 share = 0

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);

  // Since the filter is removed, resources are offered to `framework2`.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (2 agents): cpus=2, mem=1024.
  // ROLE1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 0.5 (cpus=1, mem=512)
  //   framework2 share = 0.5 (cpus=1, mem=512)
}


// This test ensures that agents which are scheduled for maintenance are
// properly sent inverse offers after they have accepted or reserved resources.
TEST_F(HierarchicalAllocatorTest, MaintenanceInverseOffers)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Create an agent.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // This framework will be offered all of the resources.
  FrameworkInfo framework = createFrameworkInfo("*");
  allocator->addFramework(framework.id(), framework, {});

  // Check that the resources go to the framework.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  const process::Time start = Clock::now() + Seconds(60);

  // Give the agent some unavailability.
  allocator->updateUnavailability(
      agent.id(),
      protobuf::maintenance::createUnavailability(
          start));

  // Check the resources get inverse offered.
  Future<Deallocation> deallocation = deallocations.get();
  AWAIT_READY(deallocation);
  EXPECT_EQ(framework.id(), deallocation.get().frameworkId);
  EXPECT_TRUE(deallocation.get().resources.contains(agent.id()));

  foreachvalue (
      const UnavailableResources& unavailableResources,
      deallocation.get().resources) {
    // The resources in the inverse offer are unspecified.
    // This means everything is being requested back.
    EXPECT_EQ(Resources(), unavailableResources.resources);

    EXPECT_EQ(
        start.duration(),
        Nanoseconds(unavailableResources.unavailability.start().nanoseconds()));
  }
}


// This test ensures that allocation is done per slave. This is done
// by having 2 slaves and 2 frameworks and making sure each framework
// gets only one slave's resources during an allocation.
TEST_F(HierarchicalAllocatorTest, CoarseGrained)
{
  // Pausing the clock ensures that the batch allocation does not
  // influence this test.
  Clock::pause();

  initialize();

  SlaveInfo slave1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), {});

  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), {});

  // Once framework1 is added, an allocation will occur. Return the
  // resources so that we can test what happens when there are 2
  // frameworks and 2 slaves to consider during allocation.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources() + slave2.resources(),
            Resources::sum(allocation.get().resources));

  allocator->recoverResources(
      framework1.id(),
      slave1.id(),
      allocation.get().resources.get(slave1.id()).get(),
      None());
  allocator->recoverResources(
      framework1.id(),
      slave2.id(),
      allocation.get().resources.get(slave2.id()).get(),
      None());

  // Now add the second framework, we expect there to be 2 subsequent
  // allocations, each framework being allocated a full slave.
  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  hashmap<FrameworkID, Allocation> frameworkAllocations;

  allocation = allocations.get();
  AWAIT_READY(allocation);
  frameworkAllocations[allocation.get().frameworkId] = allocation.get();

  allocation = allocations.get();
  AWAIT_READY(allocation);
  frameworkAllocations[allocation.get().frameworkId] = allocation.get();

  // NOTE: `slave1` and `slave2` have the same resources, we don't care
  // which framework received which slave, only that they each received one.
  ASSERT_TRUE(frameworkAllocations.contains(framework1.id()));
  ASSERT_EQ(1u, frameworkAllocations[framework1.id()].resources.size());
  EXPECT_EQ(slave1.resources(),
            Resources::sum(frameworkAllocations[framework1.id()].resources));

  ASSERT_TRUE(frameworkAllocations.contains(framework2.id()));
  ASSERT_EQ(1u, frameworkAllocations[framework2.id()].resources.size());
  EXPECT_EQ(slave2.resources(),
            Resources::sum(frameworkAllocations[framework2.id()].resources));
}


// This test ensures that frameworks that have the same share get an
// equal number of allocations over time (rather than the same
// framework getting all the allocations because it's name is
// lexicographically ordered first).
TEST_F(HierarchicalAllocatorTest, SameShareFairness)
{
  Clock::pause();

  initialize();

  FrameworkInfo framework1 = createFrameworkInfo("*");
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo("*");
  allocator->addFramework(framework2.id(), framework2, {});

  SlaveInfo slave = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Ensure that the slave's resources are alternated between both
  // frameworks.
  hashmap<FrameworkID, size_t> counts;

  for (int i = 0; i < 10; i++) {
    Future<Allocation> allocation = allocations.get();
    AWAIT_READY(allocation);
    counts[allocation.get().frameworkId]++;

    ASSERT_EQ(1u, allocation.get().resources.size());
    EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

    allocator->recoverResources(
        allocation.get().frameworkId,
        slave.id(),
        allocation.get().resources.get(slave.id()).get(),
        None());

    Clock::advance(flags.allocation_interval);
  }

  EXPECT_EQ(5u, counts[framework1.id()]);
  EXPECT_EQ(5u, counts[framework2.id()]);
}


// Checks that resources on a slave that are statically reserved to
// a role are only offered to frameworks in that role.
TEST_F(HierarchicalAllocatorTest, Reservations)
{
  Clock::pause();

  initialize();

  SlaveInfo slave1 = createSlaveInfo(
      "cpus(role1):2;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), {});

  SlaveInfo slave2 = createSlaveInfo(
      "cpus(role2):2;mem(role2):1024;cpus:1;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), {});

  // This slave's resources should never be allocated, since there
  // is no framework for role3.
  SlaveInfo slave3 = createSlaveInfo(
      "cpus(role3):1;mem(role3):1024;disk(role3):0");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), {});

  // framework1 should get all the resources from slave1, and the
  // unreserved resources from slave2.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(2u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave1.id()));
  EXPECT_TRUE(allocation.get().resources.contains(slave2.id()));
  EXPECT_EQ(slave1.resources() + Resources(slave2.resources()).unreserved(),
            Resources::sum(allocation.get().resources));

  // framework2 should get all of its reserved resources on slave2.
  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave2.id()));
  EXPECT_EQ(Resources(slave2.resources()).reserved("role2"),
            Resources::sum(allocation.get().resources));
}


// Checks that recovered resources are re-allocated correctly.
TEST_F(HierarchicalAllocatorTest, RecoverResources)
{
  Clock::pause();

  initialize();

  SlaveInfo slave = createSlaveInfo(
      "cpus(role1):1;mem(role1):200;"
      "cpus:1;mem:200;disk:0");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Initially, all the resources are allocated.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Recover the reserved resources, expect them to be re-offered.
  Resources reserved = Resources(slave.resources()).reserved("role1");

  allocator->recoverResources(
      allocation.get().frameworkId,
      slave.id(),
      reserved,
      None());

  Clock::advance(flags.allocation_interval);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(reserved, Resources::sum(allocation.get().resources));

  // Recover the unreserved resources, expect them to be re-offered.
  Resources unreserved = Resources(slave.resources()).unreserved();

  allocator->recoverResources(
      allocation.get().frameworkId,
      slave.id(),
      unreserved,
      None());

  Clock::advance(flags.allocation_interval);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(unreserved, Resources::sum(allocation.get().resources));
}


TEST_F(HierarchicalAllocatorTest, Allocatable)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  // Not enough memory or cpu to be considered allocatable.
  SlaveInfo slave1 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS / 2) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), {});

  // Enough cpus to be considered allocatable.
  SlaveInfo slave2 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave2.id()));
  EXPECT_EQ(slave2.resources(), Resources::sum(allocation.get().resources));

  // Enough memory to be considered allocatable.
  SlaveInfo slave3 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS / 2) + ";"
      "mem:" + stringify((MIN_MEM).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave3.id()));
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // slave4 has enough cpu and memory to be considered allocatable,
  // but it lies across unreserved and reserved resources!
  SlaveInfo slave4 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS / 1.5) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "cpus(role1):" + stringify(MIN_CPUS / 1.5) + ";"
      "mem(role1):" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave4.id()));
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));
}


// This test ensures that frameworks can apply offer operations (e.g.,
// creating persistent volumes) on their allocations.
TEST_F(HierarchicalAllocatorTest, UpdateAllocation)
{
  Clock::pause();

  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Initially, all the resources are allocated.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Construct an offer operation for the framework's allocation.
  Resource volume = Resources::parse("disk", "5", "*").get();
  volume.mutable_disk()->mutable_persistence()->set_id("ID");
  volume.mutable_disk()->mutable_volume()->set_container_path("data");

  Offer::Operation create;
  create.set_type(Offer::Operation::CREATE);
  create.mutable_create()->add_volumes()->CopyFrom(volume);

  // Ensure the offer operation can be applied.
  Try<Resources> updated =
    Resources::sum(allocation.get().resources).apply(create);

  ASSERT_SOME(updated);

  // Update the allocation in the allocator.
  allocator->updateAllocation(
      framework.id(),
      slave.id(),
      {create});

  // Now recover the resources, and expect the next allocation to
  // contain the updated resources.
  allocator->recoverResources(
      framework.id(),
      slave.id(),
      updated.get(),
      None());

  Clock::advance(flags.allocation_interval);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));

  // The allocation should be the slave's resources with the offer
  // operation applied.
  updated = Resources(slave.resources()).apply(create);
  ASSERT_SOME(updated);

  EXPECT_NE(Resources(slave.resources()),
            Resources::sum(allocation.get().resources));

  EXPECT_EQ(updated.get(), Resources::sum(allocation.get().resources));
}


// This test ensures that a call to 'updateAvailable' succeeds when the
// allocator has sufficient available resources.
TEST_F(HierarchicalAllocatorTest, UpdateAvailableSuccess)
{
  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Construct an offer operation for the framework's allocation.
  Resources unreserved = Resources::parse("cpus:25;mem:50").get();
  Resources dynamicallyReserved =
    unreserved.flatten("role1", createReservationInfo("ops"));

  Offer::Operation reserve = RESERVE(dynamicallyReserved);

  // Update the allocation in the allocator.
  Future<Nothing> update = allocator->updateAvailable(slave.id(), {reserve});
  AWAIT_EXPECT_READY(update);

  // Expect to receive the updated available resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));

  // The allocation should be the slave's resources with the offer
  // operation applied.
  Try<Resources> updated = Resources(slave.resources()).apply(reserve);
  ASSERT_SOME(updated);

  EXPECT_NE(Resources(slave.resources()),
            Resources::sum(allocation.get().resources));

  EXPECT_EQ(updated.get(), Resources::sum(allocation.get().resources));
}


// This test ensures that a call to 'updateAvailable' fails when the
// allocator has insufficient available resources.
TEST_F(HierarchicalAllocatorTest, UpdateAvailableFail)
{
  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Expect to receive the all of the available resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Construct an offer operation for the framework's allocation.
  Resources unreserved = Resources::parse("cpus:25;mem:50").get();
  Resources dynamicallyReserved =
    unreserved.flatten("role1", createReservationInfo("ops"));

  Offer::Operation reserve = RESERVE(dynamicallyReserved);

  // Update the allocation in the allocator.
  Future<Nothing> update = allocator->updateAvailable(slave.id(), {reserve});
  AWAIT_EXPECT_FAILED(update);
}

// This test ensures that when oversubscribed resources are updated
// subsequent allocations properly account for that.
TEST_F(HierarchicalAllocatorTest, UpdateSlave)
{
  // Pause clock to disable batch allocation.
  Clock::pause();

  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Add a framework that can accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo(
      "role1",
      {FrameworkInfo::Capability::REVOCABLE_RESOURCES});
  allocator->addFramework(framework.id(), framework, {});

  // Initially, all the resources are allocated.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Update the slave with 10 oversubscribed cpus.
  Resources oversubscribed = createRevocableResources("cpus", "10");
  allocator->updateSlave(slave.id(), oversubscribed);

  // The next allocation should be for 10 oversubscribed resources.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(oversubscribed, Resources::sum(allocation.get().resources));

  // Update the slave again with 12 oversubscribed cpus.
  Resources oversubscribed2 = createRevocableResources("cpus", "12");
  allocator->updateSlave(slave.id(), oversubscribed2);

  // The next allocation should be for 2 oversubscribed cpus.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(oversubscribed2 - oversubscribed,
            Resources::sum(allocation.get().resources));

  // Update the slave again with 5 oversubscribed cpus.
  Resources oversubscribed3 = createRevocableResources("cpus", "5");
  allocator->updateSlave(slave.id(), oversubscribed3);

  // Since there are no more available oversubscribed resources there
  // shouldn't be an allocation.
  Clock::settle();
  allocation = allocations.get();
  ASSERT_TRUE(allocation.isPending());
}


// This test verifies that a framework that has not opted in for
// revocable resources do not get allocated oversubscribed resources.
TEST_F(HierarchicalAllocatorTest, OversubscribedNotAllocated)
{
  // Pause clock to disable batch allocation.
  Clock::pause();

  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Add a framework that does *not* accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  // Initially, all the resources are allocated.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Update the slave with 10 oversubscribed cpus.
  Resources oversubscribed = createRevocableResources("cpus", "10");
  allocator->updateSlave(slave.id(), oversubscribed);

  // No allocation should be made for oversubscribed resources because
  // the framework has not opted in for them.
  Clock::settle();
  allocation = allocations.get();
  ASSERT_TRUE(allocation.isPending());
}


// This test verifies that when oversubscribed resources are partially
// recovered subsequent allocation properly accounts for that.
TEST_F(HierarchicalAllocatorTest, RecoverOversubscribedResources)
{
  // Pause clock to disable batch allocation.
  Clock::pause();

  initialize();

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  // Add a framework that can accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo(
      "role1",
      {FrameworkInfo::Capability::REVOCABLE_RESOURCES});
  allocator->addFramework(framework.id(), framework, {});

  // Initially, all the resources are allocated.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));

  // Update the slave with 10 oversubscribed cpus.
  Resources oversubscribed = createRevocableResources("cpus", "10");
  allocator->updateSlave(slave.id(), oversubscribed);

  // The next allocation should be for 10 oversubscribed cpus.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(oversubscribed, Resources::sum(allocation.get().resources));

  // Recover 6 oversubscribed cpus and 2 regular cpus.
  Resources recovered = createRevocableResources("cpus", "6");
  recovered += Resources::parse("cpus:2").get();

  allocator->recoverResources(framework.id(), slave.id(), recovered, None());

  Clock::advance(flags.allocation_interval);

  // The next allocation should be for 6 oversubscribed and 2 regular
  // cpus.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(recovered, Resources::sum(allocation.get().resources));
}


// Checks that a slave that is not whitelisted will not have its
// resources get offered, and that if the whitelist is updated so
// that it is whitelisted, its resources will then be offered.
TEST_F(HierarchicalAllocatorTest, Whitelist)
{
  Clock::pause();

  initialize();

  hashset<string> whitelist;
  whitelist.insert("dummy-agent");

  allocator->updateWhitelist(whitelist);

  SlaveInfo slave = createSlaveInfo("cpus:2;mem:1024");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});

  FrameworkInfo framework = createFrameworkInfo("*");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();

  // Ensure a batch allocation is triggered.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // There should be no allocation!
  ASSERT_TRUE(allocation.isPending());

  // Updating the whitelist to include the slave should
  // trigger an allocation in the next batch.
  whitelist.insert(slave.hostname());
  allocator->updateWhitelist(whitelist);

  Clock::advance(flags.allocation_interval);

  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(slave.resources(), Resources::sum(allocation.get().resources));
}


// The quota tests that are specific to the built-in Hierarchical DRF
// allocator (i.e. the way quota is satisfied) are in this file.

// TODO(alexr): Additional tests we may want to implement:
//   * A role has running tasks, quota is being set and is less than the
//     current allocation, some tasks finish or are killed, but the role
//     does not get new non-revocable offers (retroactively).
//   * Multiple frameworks in a role with quota set, some agents fail,
//     frameworks should be deprived fairly.
//   * Multiple quota'ed roles, some agents fail, roles should be deprived
//     according to their weights.
//   * Oversubscribed resources should not count towards quota.
//   * A role has dynamic reservations, quota is set and is less than total
//     dynamic reservations.
//   * A role has dynamic reservations, quota is set and is greater than
//     total dynamic reservations. Resource math should account them towards
//     quota and do not offer extra resources, offer dynamically reserved
//     resources as part of quota and do not re-offer them afterwards.

// In the presence of quota'ed and non-quota'ed roles, if a framework in
// the quota'ed role declines offers, some resources are laid away for
// the role, so that a greedy framework from a non-quota'ed role cannot
// eat up all free resources.
TEST_F(HierarchicalAllocatorTest, QuotaProvidesGuarantee)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // Create `framework1` and set quota for its role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  // `framework1` will be offered all of `agent1`'s resources because it is
  // the only framework in the only role with unsatisfied quota.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=1, mem=512.
  // QUOTA_ROLE share = 1 (cpus=1, mem=512) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // `framework1` will again be offered all of `agent2`'s resources
  // because it is the only framework in the only role with unsatisfied
  // quota. `framework2` has to wait.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 1 (cpus=2, mem=1024) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Now `framework1` declines the second offer and sets a filter for twice
  // the allocation interval. The declined resources should not be offered
  // to `framework2` because by doing so they may not be available to
  // `framework1` when the filter expires.
  Duration filterTimeout = flags.allocation_interval * 2;
  Filters offerFilter;
  offerFilter.set_refuse_seconds(filterTimeout.secs());

  allocator->recoverResources(
      framework1.id(),
      agent2.id(),
      allocation.get().resources.get(agent2.id()).get(),
      offerFilter);

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Ensure the offer filter timeout is set before advancing the clock.
  Clock::settle();

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // There should be no allocation due to the offer filter.
  allocation = allocations.get();
  ASSERT_TRUE(allocation.isPending());

  // Ensure the offer filter times out (2x the allocation interval)
  // and the next batch allocation occurs.
  Clock::advance(flags.allocation_interval);

  // Previously declined resources should be offered to the quota'ed role.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 1 (cpus=2, mem=1024) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0
}


// If quota is removed, fair sharing should be restored in the cluster
// after sufficient number of tasks finish.
TEST_F(HierarchicalAllocatorTest, RemoveQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota);

  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      {std::make_pair(framework1.id(), agent2.resources())});

  // Total cluster resources (2 identical agents): cpus=2, mem=1024.
  // QUOTA_ROLE share = 1 (cpus=2, mem=1024) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // All cluster resources are now being used by `framework1` as part of
  // its role quota, no further allocations are expected. However, once the
  // quota is removed, quota guarantee does not apply any more and released
  // resources should be offered to `framework2` to restore fairness.

  allocator->removeQuota(QUOTA_ROLE);

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  allocator->recoverResources(
      framework1.id(),
      agent1.id(),
      agent1.resources(),
      None());

  // Trigger the next batch allocation.
  Clock::advance(flags.allocation_interval);

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework2 share = 1

  JSON::Object metrics = Metrics();

  string metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/cpus"
    "/offered_or_allocated";
  EXPECT_EQ(0u, metrics.values.count(metric));

  metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/mem"
    "/offered_or_allocated";
  EXPECT_EQ(0u, metrics.values.count(metric));
}


// If a quota'ed role contains multiple frameworks, the resources should
// be distributed fairly between them. However, inside the quota'ed role,
// if one framework declines resources, there is no guarantee the other
// framework in the same role does not consume all role's quota.
TEST_F(HierarchicalAllocatorTest, MultipleFrameworksInRoleWithQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // Create `framework1a` and set quota for its role.
  FrameworkInfo framework1a = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1a.id(), framework1a, {});

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:4;mem:2048");
  allocator->setQuota(QUOTA_ROLE, quota);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  // `framework1a` will be offered all of `agent1`'s resources because
  // it is the only framework in the only role with unsatisfied quota.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1a.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=1, mem=512.
  // QUOTA_ROLE share = 1 (cpus=1, mem=512) [quota: cpus=2, mem=1024]
  //   framework1a share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Create `framework1b` in the quota'ed role.
  FrameworkInfo framework1b = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1b.id(), framework1b, {});

  SlaveInfo agent2 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // `framework1b` will be offered all of `agent2`'s resources
  // (coarse-grained allocation) because its share is 0 and it belongs
  // to a role with unsatisfied quota.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1b.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=3, mem=1536.
  // QUOTA_ROLE share = 1 (cpus=3, mem=1536) [quota: cpus=4, mem=2048]
  //   framework1a share = 0.33 (cpus=1, mem=512)
  //   framework1b share = 0.66 (cpus=2, mem=1024)
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  SlaveInfo agent3 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent3.id(), agent3, None(), agent3.resources(), {});

  // `framework1a` will be offered all of `agent3`'s resources because
  // its share is less than `framework1b`'s and `QUOTA_ROLE` still
  // has unsatisfied quota.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1a.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent3.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=4, mem=2048.
  // QUOTA_ROLE share = 1 (cpus=4, mem=2048) [quota: cpus=4, mem=2048]
  //   framework1a share = 0.5 (cpus=2, mem=1024)
  //   framework1b share = 0.5 (cpus=2, mem=1024)
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // If `framework1a` declines offered resources, they will be allocated to
  // `framework1b`.
  Filters filter5s;
  filter5s.set_refuse_seconds(5.);
  allocator->recoverResources(
      framework1a.id(),
      agent3.id(),
      agent3.resources(),
      filter5s);

  // Trigger the next batch allocation.
  Clock::advance(flags.allocation_interval);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1b.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent3.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=4, mem=2048.
  // QUOTA_ROLE share = 1 (cpus=4, mem=2048) [quota: cpus=4, mem=2048]
  //   framework1a share = 0.25 (cpus=1, mem=512)
  //   framework1b share = 0.75 (cpus=3, mem=1536)
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0
}


// The allocator performs coarse-grained allocations, and allocations
// to satisfy quota are no exception. A role may get more resources as
// part of its quota if the agent remaining resources are greater than
// the unsatisfied part of the role's quota.
TEST_F(HierarchicalAllocatorTest, QuotaAllocationGranularity)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // Create `framework1` and set quota for its role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  // Set quota to be less than the agent resources.
  const Quota quota = createQuota(QUOTA_ROLE, "cpus:0.5;mem:200");
  allocator->setQuota(QUOTA_ROLE, quota);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // `framework1` will be offered all of `agent`'s resources because
  // it is the only framework in the only role with unsatisfied quota
  // and the allocator performs coarse-grained allocation.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));
  EXPECT_TRUE(Resources(agent.resources()).contains(quota.info.guarantee()));

  // Total cluster resources: cpus=1, mem=512.
  // QUOTA_ROLE share = 1 (cpus=1, mem=512) [quota: cpus=0.5, mem=200]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0
}


// This test verifies, that the free pool (what is left after all quotas
// are satisfied) is allocated according to the DRF algorithm across the roles
// which do not have quota set.
TEST_F(HierarchicalAllocatorTest, DRFWithQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:0.25;mem:128");
  allocator->setQuota(QUOTA_ROLE, quota);

  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  JSON::Object metrics = Metrics();

  string metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/cpus"
    "/guarantee";
  EXPECT_EQ(0.25, metrics.values[metric]);

  metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/mem"
    "/guarantee";
  EXPECT_EQ(128, metrics.values[metric]);

  // Add an agent with some allocated resources.
  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), Resources(quota.info.guarantee()))});

  // Total cluster resources (1 agent): cpus=1, mem=512.
  // QUOTA_ROLE share = 0.25 (cpus=0.25, mem=128) [quota: cpus=0.25, mem=128]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Some resources on `agent1` are now being used by `framework1` as part
  // of its role quota. All quotas are satisfied, all available resources
  // should be allocated according to fair shares of roles and frameworks.

  // `framework2` will be offered all of `agent1`'s resources because its
  // share is 0.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources() - Resources(quota.info.guarantee()),
            Resources::sum(allocation.get().resources));

  metrics = Metrics();

  metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/cpus"
    "/offered_or_allocated";
  EXPECT_EQ(0.25, metrics.values[metric]);

  metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/mem"
    "/offered_or_allocated";
  EXPECT_EQ(128, metrics.values[metric]);

  metric =
    "allocator/mesos/quota"
    "/roles/" + QUOTA_ROLE +
    "/resources/disk"
    "/offered_or_allocated";
  EXPECT_EQ(0u, metrics.values.count(metric));

  // Total cluster resources (1 agent): cpus=1, mem=512.
  // QUOTA_ROLE share = 0.25 (cpus=0.25, mem=128) [quota: cpus=0.25, mem=128]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0.75 (cpus=0.75, mem=384)
  //   framework2 share = 1

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // `framework2` will be offered all of `agent2`'s resources (coarse-grained
  // allocation). `framework1` does not receive them even though it has a
  // smaller allocation, since we have already satisfied its role's quota.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));
}


// This tests addresses a so-called "starvation" case. Suppose there are
// several frameworks below their fair share: they decline any offers they
// get. There is also a framework which fully utilizes its share and would
// accept more resources if they were offered. However, if there are not
// many free resources available and the decline timeout is small enough,
// free resources may circulate between frameworks underutilizing their fair
// share and might never be offered to the framework that needs them. While
// this behavior corresponds to the way DRF algorithm works, it might not be
// desirable in some cases. Setting quota for a "starving" role can mitigate
// the issue.
TEST_F(HierarchicalAllocatorTest, QuotaAgainstStarvation)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because all resources are already allocated.
  Clock::settle();

  // Total cluster resources (1 agent): cpus=1, mem=512.
  // QUOTA_ROLE share = 1 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // Free cluster resources on `agent2` will be allocated to `framework2`
  // because its share is 0.

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (2 identical agents): cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework2 share = 1

  // If `framework2` declines offered resources with 0 timeout, they will
  // be returned to the free pool and then allocated to `framework2` again,
  // because its share is still 0.
  Filters filter0s;
  filter0s.set_refuse_seconds(0.);
  allocator->recoverResources(
      framework2.id(),
      agent2.id(),
      agent2.resources(),
      filter0s);

  // Total cluster resources (2 identical agents): cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Trigger the next batch allocation.
  Clock::advance(flags.allocation_interval);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // `framework2` continues declining offers.
  allocator->recoverResources(
      framework2.id(),
      agent2.id(),
      agent2.resources(),
      filter0s);

  // We set quota for the "starving" `QUOTA_ROLE` role.
  const Quota quota = createQuota(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota);

  // Since `QUOTA_ROLE` is under quota, `agent2`'s resources will
  // be allocated to `framework1`.

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 1 (cpus=2, mem=1024) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0
}


// This test checks that quota is respected even for roles that do not
// have any frameworks currently registered. It also ensures an event-
// triggered allocation does not unnecessarily deprive non-quota'ed
// frameworks of resources.
TEST_F(HierarchicalAllocatorTest, QuotaAbsentFramework)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // Set quota for the quota'ed role. This role isn't registered with
  // the allocator yet.
  const Quota quota = createQuota(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota);

  // Add `framework` in the non-quota'ed role.
  FrameworkInfo framework = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework.id(), framework, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  // Total cluster resources (0 agents): 0.
  // QUOTA_ROLE share = 0 [quota: cpus=2, mem=1024]
  //   no frameworks
  // NO_QUOTA_ROLE share = 0
  //   framework share = 0

  // Each `addSlave()` triggers an event-based allocation.
  //
  // NOTE: The second event-based allocation for `agent2` takes into account
  // that `agent1`'s resources are laid away for `QUOTA_ROLE`'s quota and
  // hence freely allocates for the non-quota'ed `NO_QUOTA_ROLE` role.
  SlaveInfo agent1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // `framework` can only be allocated resources on `agent2`. This
  // is due to the coarse-grained nature of the allocations. All the
  // free resources on `agent1` would be considered to construct an
  // offer, and that would exceed the resources allowed to be offered
  // to the non-quota'ed role.
  //
  // NOTE: We would prefer to test that, without the presence of
  // `agent2`, `framework` is not allocated anything. However, we
  // can't easily test for the absence of an allocation from the
  // framework side, so we make due with this instead.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (2 agents): cpus=3, mem=1536.
  // QUOTA_ROLE share = 0 [quota: cpus=2, mem=1024], but
  //                    (cpus=2, mem=1024) are laid away
  //   no frameworks
  // NO_QUOTA_ROLE share = 0.33
  //   framework share = 1 (cpus=1, mem=512)
}


// This test checks that if one role with quota has no frameworks in it,
// other roles with quota are still offered resources. Roles without
// frameworks have zero fair share and are always considered first during
// allocation, hence this test actually addresses several scenarios:
//  * Quota'ed roles without frameworks do not prevent other quota'ed roles
//    from getting resources.
//  * Resources are not laid away for quota'ed roles without frameworks if
//    there are other quota'ed roles with not fully satisfied quota.
TEST_F(HierarchicalAllocatorTest, MultiQuotaAbsentFrameworks)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE1{"quota-role-1"};
  const string QUOTA_ROLE2{"quota-role-2"};

  initialize();

  SlaveInfo agent = createSlaveInfo("cpus:2;mem:2048;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // Set quota for both roles.
  const Quota quota1 = createQuota(QUOTA_ROLE1, "cpus:1;mem:1024");
  allocator->setQuota(QUOTA_ROLE1, quota1);

  const Quota quota2 = createQuota(QUOTA_ROLE2, "cpus:2;mem:2048");
  allocator->setQuota(QUOTA_ROLE2, quota2);

  // Add a framework in the `QUOTA_ROLE2` role.
  FrameworkInfo framework = createFrameworkInfo(QUOTA_ROLE2);
  allocator->addFramework(framework.id(), framework, {});

  // Due to the coarse-grained nature of the allocations, `framework` will
  // get all `agent`'s resources.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));
}


// This test checks that if there are multiple roles with quota, all of them
// get enough offers given there are enough resources. Suppose one quota'ed
// role has smaller share and is fully satisfied. Another quota'ed role has
// greater share but its quota is not fully satisfied yet. Though the first
// role is considered before the second because it has smaller share, this
// should not lead to starvation of the second role.
TEST_F(HierarchicalAllocatorTest, MultiQuotaWithFrameworks)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE1{"quota-role-1"};
  const string QUOTA_ROLE2{"quota-role-2"};

  initialize();

  // Mem Quota for `QUOTA_ROLE1` is 10 times smaller than for `QUOTA_ROLE2`.
  const Quota quota1 = createQuota(QUOTA_ROLE1, "cpus:1;mem:200");
  allocator->setQuota(QUOTA_ROLE1, quota1);

  const Quota quota2 = createQuota(QUOTA_ROLE2, "cpus:2;mem:2000");
  allocator->setQuota(QUOTA_ROLE2, quota2);

  // Add `framework1` in the `QUOTA_ROLE1` role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE1);
  allocator->addFramework(framework1.id(), framework1, {});

  // Add `framework2` in the `QUOTA_ROLE2` role.
  FrameworkInfo framework2 = createFrameworkInfo(QUOTA_ROLE2);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:1024;disk:0");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:1024;disk:0");
  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      {std::make_pair(framework2.id(), agent2.resources())});

  // Total cluster resources (2 identical agents): cpus=2, mem=2048.
  // QUOTA_ROLE1 share = 0.5 (cpus=1, mem=1024) [quota: cpus=1, mem=200]
  //   framework1 share = 1
  // QUOTA_ROLE2 share = 0.5 (cpus=1, mem=1024) [quota: cpus=2, mem=2000]
  //   framework2 share = 1

  // Quota for the `QUOTA_ROLE1` role is satisfied, while `QUOTA_ROLE2` is
  // under quota. Hence resources of the newly added agent should be offered
  // to the framework in `QUOTA_ROLE2`.

  SlaveInfo agent3 = createSlaveInfo("cpus:2;mem:2048");
  allocator->addSlave(agent3.id(), agent3, None(), agent3.resources(), {});

  // `framework2` will get all agent3's resources because its role is under
  // quota, while other roles' quotas are satisfied.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent3.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources (3 agents): cpus=4, mem=4096.
  // QUOTA_ROLE1 share = 0.25 (cpus=1, mem=1024) [quota: cpus=1, mem=200]
  //   framework1 share = 1
  // QUOTA_ROLE2 share = 0.75 (cpus=3, mem=3072) [quota: cpus=2, mem=2000]
  //   framework2 share = 1
}


// This tests that reserved resources are accounted for in the role's quota.
TEST_F(HierarchicalAllocatorTest, ReservationWithinQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NON_QUOTA_ROLE{"non-quota-role"};

  initialize();

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:2;mem:256");
  allocator->setQuota(QUOTA_ROLE, quota);

  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  FrameworkInfo framework2 = createFrameworkInfo(NON_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // Process all triggered allocation events.
  //
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  // Some resources on `agent1` are now being used by `framework1` as part
  // of its role quota. `framework2` will be offered the rest of `agent1`'s
  // resources since `framework1`'s quota is satisfied, and `framework2` has
  // no resources.
  SlaveInfo agent1 = createSlaveInfo("cpus:8;mem(" + QUOTA_ROLE + "):256");
  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(
          framework1.id(),
          // The `mem` portion is used to test that reserved resources are
          // accounted for, and the `cpus` portion is allocated to show that
          // the result of DRF would be different if `mem` was not accounted.
          Resources::parse("cpus:2;mem(" + QUOTA_ROLE + "):256").get())});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);

  EXPECT_EQ(Resources::parse("cpus:6").get(),
            Resources::sum(allocation.get().resources));

  // Since the reserved resources account towards the quota as well as being
  // accounted for DRF, we expect these resources to also be allocated to
  // `framework2`.
  SlaveInfo agent2 = createSlaveInfo("cpus:4");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);

  EXPECT_EQ(Resources::parse("cpus:4").get(),
            Resources::sum(allocation.get().resources));
}


// This test checks that when setting aside unallocated resources to
// ensure that a quota guarantee can be met, we don't use resources
// that have been reserved for a different role.
//
// We setup a scenario with 8 CPUs, where role X has quota for 4 CPUs
// and role Y has 4 CPUs reserved. All offers are declined; the 4
// unreserved CPUs should not be offered to role Y.
TEST_F(HierarchicalAllocatorTest, QuotaSetAsideReservedResources)
{
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // Create two agents.
  SlaveInfo agent1 = createSlaveInfo("cpus:4;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  SlaveInfo agent2 = createSlaveInfo("cpus:4;mem:512;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});

  // Reserve 4 CPUs and 512MB of memory on `agent2` for non-quota'ed role.
  Resources unreserved = Resources::parse("cpus:4;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.flatten(NO_QUOTA_ROLE, createReservationInfo("ops"));

  Offer::Operation reserve = RESERVE(dynamicallyReserved);

  Future<Nothing> updateAgent2 =
    allocator->updateAvailable(agent2.id(), {reserve});

  AWAIT_EXPECT_READY(updateAgent2);

  // Create `framework1` and set quota for its role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(framework1.id(), framework1, {});

  const Quota quota = createQuota(QUOTA_ROLE, "cpus:4");
  allocator->setQuota(QUOTA_ROLE, quota);

  // `framework1` will be offered resources at `agent1` because the
  // resources at `agent2` are reserved for a different role.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // `framework1` declines the resources on `agent1` for the duration
  // of the test.
  Filters longFilter;
  longFilter.set_refuse_seconds(flags.allocation_interval.secs() * 10);

  allocator->recoverResources(
      framework1.id(),
      agent1.id(),
      agent1.resources(),
      longFilter);

  // Trigger a batch allocation for good measure, but don't expect any
  // allocations.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  allocation = allocations.get();
  EXPECT_TRUE(allocation.isPending());

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(framework2.id(), framework2, {});

  // `framework2` will be offered the reserved resources at `agent2`
  // because those resources are reserved for its role.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(dynamicallyReserved, Resources::sum(allocation.get().resources));

  // `framework2` declines the resources on `agent2` for the duration
  // of the test.
  allocator->recoverResources(
      framework2.id(),
      agent2.id(),
      dynamicallyReserved,
      longFilter);

  // No more resource offers should be made until the filters expire:
  // `framework1` should not be offered the resources at `agent2`
  // (because they are reserved for a different role), and
  // `framework2` should not be offered the resources at `agent1`
  // (because this would risk violating quota guarantees).

  // Trigger a batch allocation for good measure, but don't expect any
  // allocations.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  allocation = allocations.get();
  EXPECT_TRUE(allocation.isPending());
}


// This test checks that if a framework suppresses offers, disconnects and
// reconnects again, it will start receiving resource offers again.
TEST_F(HierarchicalAllocatorTest, DeactivateAndReactivateFramework)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Total cluster resources will become cpus=2, mem=1024.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // Framework will be offered all of the agent's resources since it is
  // the only framework running so far.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  allocator->recoverResources(
      framework.id(),
      agent.id(),
      agent.resources(),
      None());

  // Suppress offers and disconnect framework.
  allocator->suppressOffers(framework.id());
  allocator->deactivateFramework(framework.id());

  // Advance the clock and trigger a background allocation cycle.
  Clock::advance(flags.allocation_interval);

  // Wait for all the `suppressOffers` and `deactivateFramework`
  // operations to be processed.
  Clock::settle();

  allocation = allocations.get();
  EXPECT_TRUE(allocation.isPending());

  // Reconnect the framework again.
  allocator->activateFramework(framework.id());

  // Framework will be offered all of agent's resources again
  // after getting activated.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));
}


// This test verifies that offer suppression and revival work as intended.
TEST_F(HierarchicalAllocatorTest, SuppressAndReviveOffers)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Total cluster resources will become cpus=2, mem=1024.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // Framework will be offered all of the agent's resources since it is
  // the only framework running so far.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  // Here the revival is totally unnecessary but we should tolerate the
  // framework's redundant REVIVE calls.
  allocator->reviveOffers(framework.id());

  // Nothing is allocated because of no additional resources.
  allocation = allocations.get();
  EXPECT_TRUE(allocation.isPending());

  allocator->recoverResources(
      framework.id(),
      agent.id(),
      agent.resources(),
      None());

  allocator->suppressOffers(framework.id());

  // Advance the clock and trigger a background allocation cycle.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // Still pending because the framework has suppressed offers.
  EXPECT_TRUE(allocation.isPending());

  // Revive again and this time it should work.
  allocator->reviveOffers(framework.id());

  // Framework will be offered all of agent's resources again after
  // reviving offers.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));
}


// This test checks that total and allocator resources
// are correctly reflected in the metrics endpoint.
TEST_F(HierarchicalAllocatorTest, ResourceMetrics)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});
  Clock::settle();

  JSON::Object expected;

  // No frameworks are registered yet, so nothing is allocated.
  expected.values = {
      {"allocator/mesos/resources/cpus/total",   2},
      {"allocator/mesos/resources/mem/total", 1024},
      {"allocator/mesos/resources/disk/total",   0},
      {"allocator/mesos/resources/cpus/offered_or_allocated", 0},
      {"allocator/mesos/resources/mem/offered_or_allocated",  0},
      {"allocator/mesos/resources/disk/offered_or_allocated", 0},
  };

  JSON::Value metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));

  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});
  Clock::settle();

  // All of the resources should be offered.
  expected.values = {
      {"allocator/mesos/resources/cpus/total",   2},
      {"allocator/mesos/resources/mem/total", 1024},
      {"allocator/mesos/resources/disk/total",   0},
      {"allocator/mesos/resources/cpus/offered_or_allocated",   2},
      {"allocator/mesos/resources/mem/offered_or_allocated", 1024},
      {"allocator/mesos/resources/disk/offered_or_allocated",   0},
  };

  metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));

  allocator->removeSlave(agent.id());
  Clock::settle();

  // No frameworks are registered yet, so nothing is allocated.
  expected.values = {
      {"allocator/mesos/resources/cpus/total", 0},
      {"allocator/mesos/resources/mem/total",  0},
      {"allocator/mesos/resources/disk/total", 0},
      {"allocator/mesos/resources/cpus/offered_or_allocated", 0},
      {"allocator/mesos/resources/mem/offered_or_allocated",  0},
      {"allocator/mesos/resources/disk/offered_or_allocated", 0},
  };

  metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));
}


// This test checks that the number of times the allocation
// algorithm has run is correctly reflected in the metric.
TEST_F(HierarchicalAllocatorTest, AllocationRunsMetric)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  size_t allocations = 0;

  JSON::Object expected;

  expected.values = { {"allocator/mesos/allocation_runs", allocations} };

  JSON::Value metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));

  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});
  ++allocations; // Adding an agent triggers allocations.

  FrameworkInfo framework = createFrameworkInfo("role");
  allocator->addFramework(framework.id(), framework, {});
  ++allocations; // Adding a framework triggers allocations.

  Clock::settle();

  expected.values = { {"allocator/mesos/allocation_runs", allocations} };

  metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));
}


// This test checks that the allocation run timer
// metrics are reported in the metrics endpoint.
TEST_F(HierarchicalAllocatorTest, AllocationRunTimerMetrics)
{
  Clock::pause();

  initialize();

  // These time series statistics will be generated
  // once at least 2 allocation runs occur.
  auto statistics = {
    "allocator/mesos/allocation_run_ms/count",
    "allocator/mesos/allocation_run_ms/min",
    "allocator/mesos/allocation_run_ms/max",
    "allocator/mesos/allocation_run_ms/p50",
    "allocator/mesos/allocation_run_ms/p95",
    "allocator/mesos/allocation_run_ms/p99",
    "allocator/mesos/allocation_run_ms/p999",
    "allocator/mesos/allocation_run_ms/p9999",
  };

  JSON::Object metrics = Metrics();
  map<string, JSON::Value> values = metrics.values;

  EXPECT_EQ(0u, values.count("allocator/mesos/allocation_run_ms"));

  // No allocation timing statistics should appear.
  foreach (const string& statistic, statistics) {
    EXPECT_EQ(0u, values.count(statistic))
      << "Expected " << statistic << " to be absent";
  }

  // Allow the allocation timer to measure time.
  Clock::resume();

  // Trigger at least two calls to allocate occur
  // to generate the window statistics.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  // Wait for the allocation to complete.
  AWAIT_READY(allocations.get());

  // Ensure the timer has been stopped so that
  // the second measurement to be recorded.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  metrics = Metrics();
  values = metrics.values;

  // A non-zero measurement should be present.
  EXPECT_EQ(1u, values.count("allocator/mesos/allocation_run_ms"));

  JSON::Value value = metrics.values["allocator/mesos/allocation_run_ms"];
  ASSERT_TRUE(value.is<JSON::Number>()) << value.which();

  JSON::Number timing = value.as<JSON::Number>();
  ASSERT_EQ(JSON::Number::FLOATING, timing.type);
  EXPECT_GT(timing.as<double>(), 0.0);

  // The statistics should be generated.
  foreach (const string& statistic, statistics) {
    EXPECT_EQ(1u, values.count(statistic))
      << "Expected " << statistic << " to be present";
  }
}


// This test checks that per-role active offer filter metrics
// are correctly reported in the metrics endpoint.
TEST_F(HierarchicalAllocatorTest, ActiveOfferFiltersMetrics)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // Register three frameworks, two of which are in the same role.
  // For every offer the frameworks install practically indefinite
  // offer filters.
  Duration filterTimeout = flags.allocation_interval * 100;
  Filters offerFilter;
  offerFilter.set_refuse_seconds(filterTimeout.secs());

  FrameworkInfo framework1 = createFrameworkInfo("roleA");
  allocator->addFramework(framework1.id(), framework1, {});

  Future<Allocation> allocation = allocations.get();

  AWAIT_READY(allocation);
  ASSERT_EQ(framework1.id(), allocation->frameworkId);

  allocator->recoverResources(
      allocation->frameworkId,
      agent.id(),
      allocation->resources.get(agent.id()).get(),
      offerFilter);

  JSON::Object expected;
  expected.values = {
      {"allocator/mesos/offer_filters/roles/roleA/active", 1},
  };

  JSON::Value metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));

  FrameworkInfo framework2 = createFrameworkInfo("roleB");
  allocator->addFramework(framework2.id(), framework2, {});

  allocation = allocations.get();

  AWAIT_READY(allocation);
  ASSERT_EQ(framework2.id(), allocation->frameworkId);

  allocator->recoverResources(
      allocation->frameworkId,
      agent.id(),
      allocation->resources.get(agent.id()).get(),
      offerFilter);

  expected.values = {
      {"allocator/mesos/offer_filters/roles/roleA/active", 1},
      {"allocator/mesos/offer_filters/roles/roleB/active", 1},
  };

  metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));

  FrameworkInfo framework3 = createFrameworkInfo("roleA");
  allocator->addFramework(framework3.id(), framework3, {});

  allocation = allocations.get();

  AWAIT_READY(allocation);
  ASSERT_EQ(framework3.id(), allocation->frameworkId);

  allocator->recoverResources(
      allocation->frameworkId,
      agent.id(),
      allocation->resources.get(agent.id()).get(),
      offerFilter);

  expected.values = {
      {"allocator/mesos/offer_filters/roles/roleA/active", 2},
      {"allocator/mesos/offer_filters/roles/roleB/active", 1},
  };

  metrics = Metrics();

  EXPECT_TRUE(metrics.contains(expected));
}


// Verifies that per-role dominant share metrics are correctly reported.
TEST_F(HierarchicalAllocatorTest, DominantShareMetrics)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Register one agent and one framework. The framework will
  // immediately receive receive an offer and make it have the
  // maximum possible dominant share.
  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:1024");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  FrameworkInfo framework1 = createFrameworkInfo("roleA");
  allocator->addFramework(framework1.id(), framework1, {});
  Clock::settle();

  JSON::Object expected;

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 1},
  };

  JSON::Value metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  // Decline the offered resources and expect a zero share.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  allocator->recoverResources(
      allocation->frameworkId,
      agent1.id(),
      allocation->resources.get(agent1.id()).get(),
      None());
  Clock::settle();

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 0},
  };

  metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  // Register a second framework. This framework will receive
  // offers as `framework1` has just declined an offer and the
  // implicit filter has not yet timed out. The new framework
  // will have the full share.
  FrameworkInfo framework2 = createFrameworkInfo("roleB");
  allocator->addFramework(framework2.id(), framework2, {});
  Clock::settle();

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 0},
      {"allocator/mesos/roles/roleB/shares/dominant", 1},
  };

  metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  // Add a second, identical agent. Now `framework1` will
  // receive an offer since it has the lowest dominant
  // share. After the offer the dominant shares of
  // `framework1` and `framework2` are equal.
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:1024");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});
  Clock::settle();

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 0.5},
      {"allocator/mesos/roles/roleB/shares/dominant", 0.5},
  };

  metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  // Removing `framework2` frees up its allocated resources. The
  // corresponding metric is removed when the last framework in
  // the role is removed.
  allocator->removeFramework(framework2.id());
  Clock::settle();

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 0.5},
  };

  metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  ASSERT_TRUE(metrics.is<JSON::Object>());
  map<string, JSON::Value> values = metrics.as<JSON::Object>().values;
  EXPECT_EQ(0u, values.count("allocator/mesos/roles/roleB/shares/dominant"));
}


// Verifies that per-role dominant share metrics are correctly
// reported when resources are excluded from fair sharing.
TEST_F(HierarchicalAllocatorTest, DominantShareMetricsWithFairnessExclusion)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  // Specify that `gpus` should not be fairly shared.
  master::Flags flags_;
  flags_.fair_sharing_excluded_resource_names = set<string>({"gpus"});

  initialize(flags_);

  // Register one agent and one framework. The framework will
  // immediately receive receive an offer and make it have the
  // maximum possible dominant share.
  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:1024;gpus:1");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), {});

  FrameworkInfo framework1 = createFrameworkInfo(
      "roleA", {FrameworkInfo::Capability::GPU_RESOURCES});

  allocator->addFramework(framework1.id(), framework1, {});
  Clock::settle();

  JSON::Object expected;

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 1},
  };

  JSON::Value metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));

  FrameworkInfo framework2 = createFrameworkInfo("roleB");
  allocator->addFramework(framework2.id(), framework2, {});
  Clock::settle();

  // Add a second, identical agent. Now `framework2` will
  // receive an offer since it has the lowest dominant share:
  // the 100% of `gpus` allocated to framework1 are excluded!
  SlaveInfo agent2 = createSlaveInfo("cpus:3;mem:3072");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), {});
  Clock::settle();

  expected.values = {
      {"allocator/mesos/roles/roleA/shares/dominant", 0.25},
      {"allocator/mesos/roles/roleB/shares/dominant", 0.75},
  };

  metrics = Metrics();
  EXPECT_TRUE(metrics.contains(expected));
}


// This test ensures that resource allocation is done according to each role's
// weight. This is done by having six agents and three frameworks and making
// sure each framework gets the appropriate number of resources.
TEST_F(HierarchicalAllocatorTest, UpdateWeight)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Define some constants to make the code read easily.
  const string SINGLE_RESOURCE = "cpus:2;mem:1024";
  const string DOUBLE_RESOURCES = "cpus:4;mem:2048";
  const string TRIPLE_RESOURCES = "cpus:6;mem:3072";
  const string FOURFOLD_RESOURCES = "cpus:8;mem:4096";
  const string TOTAL_RESOURCES = "cpus:12;mem:6144";

  auto awaitAllocationsAndRecoverResources = [this](
      Resources& totalAllocatedResources,
      hashmap<FrameworkID, Allocation>& frameworkAllocations,
      int allocationsCount,
      bool recoverResources) {
    for (int i = 0; i < allocationsCount; i++) {
      Future<Allocation> allocation = allocations.get();
      AWAIT_READY(allocation);

      frameworkAllocations[allocation.get().frameworkId] = allocation.get();
      totalAllocatedResources += Resources::sum(allocation.get().resources);

      if (recoverResources) {
        // Recover the allocated resources so they can be offered
        // again next time.
        foreachpair (const SlaveID& slaveId,
                     const Resources& resources,
                     allocation.get().resources) {
          allocator->recoverResources(
              allocation.get().frameworkId,
              slaveId,
              resources,
              None());
        }
      }
    }
  };

  // Register six agents with the same resources (cpus:2;mem:1024).
  vector<SlaveInfo> agents;
  for (unsigned i = 0; i < 6; i++) {
    SlaveInfo agent = createSlaveInfo(SINGLE_RESOURCE);
    agents.push_back(agent);
    allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});
  }

  // Total cluster resources (6 agents): cpus=12, mem=6144.

  // Framework1 registers with 'role1' which uses the default weight (1.0),
  // and all resources will be offered to this framework since it is the only
  // framework running so far.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, {});

  // Framework2 registers with 'role2' which also uses the default weight.
  // It will not get any offers due to all resources having outstanding offers
  // to framework1 when it registered.
  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);

  // role1 share = 1 (cpus=12, mem=6144)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0

  ASSERT_EQ(allocation.get().frameworkId, framework1.id());
  ASSERT_EQ(6u, allocation.get().resources.size());
  EXPECT_EQ(Resources::parse(TOTAL_RESOURCES).get(),
            Resources::sum(allocation.get().resources));

  // Recover all resources so they can be offered again next time.
  foreachpair (const SlaveID& slaveId,
               const Resources& resources,
               allocation.get().resources) {
    allocator->recoverResources(
        allocation.get().frameworkId,
        slaveId,
        resources,
        None());
  }

  // Tests whether `framework1` and `framework2` each get half of the resources
  // when their roles' weights are 1:1.
  {
    // Advance the clock and trigger a batch allocation.
    Clock::advance(flags.allocation_interval);

    // role1 share = 0.5 (cpus=6, mem=3072)
    //   framework1 share = 1
    // role2 share = 0.5 (cpus=6, mem=3072)
    //   framework2 share = 1

    // Ensure that all resources are offered equally between both frameworks,
    // since each framework's role has a weight of 1.0 by default.
    hashmap<FrameworkID, Allocation> frameworkAllocations;
    Resources totalAllocatedResources;
    awaitAllocationsAndRecoverResources(totalAllocatedResources,
        frameworkAllocations, 2, true);

    // Framework1 should get one allocation with three agents.
    ASSERT_EQ(3u, frameworkAllocations[framework1.id()].resources.size());
    EXPECT_EQ(Resources::parse(TRIPLE_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework1.id()].resources));

    // Framework2 should also get one allocation with three agents.
    ASSERT_EQ(3u, frameworkAllocations[framework2.id()].resources.size());
    EXPECT_EQ(Resources::parse(TRIPLE_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework2.id()].resources));

    // Check to ensure that these two allocations sum to the total resources;
    // this check can ensure there are only two allocations in this case.
    EXPECT_EQ(Resources::parse(TOTAL_RESOURCES).get(), totalAllocatedResources);
  }

  // Tests whether `framework1` gets 1/3 of the resources and `framework2` gets
  // 2/3 of the resources when their roles' weights are 1:2.
  {
    // Update the weight of framework2's role to 2.0.
    vector<WeightInfo> weightInfos;
    weightInfos.push_back(createWeightInfo(framework2.role(), 2.0));
    allocator->updateWeights(weightInfos);

    // 'updateWeights' will trigger the allocation immediately, so it does not
    // need to manually advance the clock here.

    // role1 share = 0.33 (cpus=4, mem=2048)
    //   framework1 share = 1
    // role2 share = 0.66 (cpus=8, mem=4096)
    //   framework2 share = 1

    // Now that the frameworks's weights are 1:2, ensure that all
    // resources are offered with a ratio of 1:2 between both frameworks.
    hashmap<FrameworkID, Allocation> frameworkAllocations;
    Resources totalAllocatedResources;
    awaitAllocationsAndRecoverResources(totalAllocatedResources,
        frameworkAllocations, 2, true);

    // Framework1 should get one allocation with two agents.
    ASSERT_EQ(2u, frameworkAllocations[framework1.id()].resources.size());
    EXPECT_EQ(Resources::parse(DOUBLE_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework1.id()].resources));

    // Framework2 should get one allocation with four agents.
    ASSERT_EQ(4u, frameworkAllocations[framework2.id()].resources.size());
    EXPECT_EQ(Resources::parse(FOURFOLD_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework2.id()].resources));

    // Check to ensure that these two allocations sum to the total resources;
    // this check can ensure there are only two allocations in this case.
    EXPECT_EQ(Resources::parse(TOTAL_RESOURCES).get(), totalAllocatedResources);
  }

  // Tests whether `framework1` gets 1/6 of the resources, `framework2` gets
  // 2/6 of the resources and `framework3` gets 3/6 of the resources when their
  // roles' weights are 1:2:3.
  {
    // Add a new role with a weight of 3.0.
    vector<WeightInfo> weightInfos;
    weightInfos.push_back(createWeightInfo("role3", 3.0));
    allocator->updateWeights(weightInfos);

    // 'updateWeights' will not trigger the allocation immediately because no
    // framework exists in 'role3' yet.

    // Framework3 registers with 'role3'.
    FrameworkInfo framework3 = createFrameworkInfo("role3");
    allocator->addFramework(framework3.id(), framework3, {});

    // 'addFramework' will trigger the allocation immediately, so it does not
    // need to manually advance the clock here.

    // role1 share = 0.166 (cpus=2, mem=1024)
    //   framework1 share = 1
    // role2 share = 0.333 (cpus=4, mem=2048)
    //   framework2 share = 1
    // role3 share = 0.50 (cpus=6, mem=3072)
    //   framework3 share = 1

    // Currently, there are three frameworks and six agents in this cluster,
    // and the weight ratio of these frameworks is 1:2:3, therefore frameworks
    // will get the proper resource ratio of 1:2:3.
    hashmap<FrameworkID, Allocation> frameworkAllocations;
    Resources totalAllocatedResources;
    awaitAllocationsAndRecoverResources(totalAllocatedResources,
        frameworkAllocations, 3, false);

    // Framework1 should get one allocation with one agent.
    ASSERT_EQ(1u, frameworkAllocations[framework1.id()].resources.size());
    EXPECT_EQ(Resources::parse(SINGLE_RESOURCE).get(),
        Resources::sum(frameworkAllocations[framework1.id()].resources));

    // Framework2 should get one allocation with two agents.
    ASSERT_EQ(2u, frameworkAllocations[framework2.id()].resources.size());
    EXPECT_EQ(Resources::parse(DOUBLE_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework2.id()].resources));

    // Framework3 should get one allocation with three agents.
    ASSERT_EQ(3u, frameworkAllocations[framework3.id()].resources.size());
    EXPECT_EQ(Resources::parse(TRIPLE_RESOURCES).get(),
        Resources::sum(frameworkAllocations[framework3.id()].resources));

    // Check to ensure that these three allocations sum to the total resources;
    // this check can ensure there are only three allocations in this case.
    EXPECT_EQ(Resources::parse(TOTAL_RESOURCES).get(), totalAllocatedResources);
  }
}


// This test checks that if a framework declines resources with a
// long filter, it will be offered filtered resources again after
// reviving offers.
TEST_F(HierarchicalAllocatorTest, ReviveOffers)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the batch allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  // Total cluster resources will become cpus=2, mem=1024.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), {});

  // Framework will be offered all of agent's resources since it is
  // the only framework running so far.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, {});

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));

  Filters filter1000s;
  filter1000s.set_refuse_seconds(1000.);
  allocator->recoverResources(
      framework.id(),
      agent.id(),
      agent.resources(),
      filter1000s);

  // Advance the clock to trigger a batch allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  allocation = allocations.get();
  EXPECT_TRUE(allocation.isPending());

  allocator->reviveOffers(framework.id());

  // Framework will be offered all of agent's resources again
  // after reviving offers.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent.resources(), Resources::sum(allocation.get().resources));
}


class HierarchicalAllocator_BENCHMARK_Test
  : public HierarchicalAllocatorTestBase,
    public WithParamInterface<std::tr1::tuple<size_t, size_t>> {};


// The Hierarchical Allocator benchmark tests are parameterized
// by the number of slaves.
INSTANTIATE_TEST_CASE_P(
    SlaveAndFrameworkCount,
    HierarchicalAllocator_BENCHMARK_Test,
    ::testing::Combine(
      ::testing::Values(1000U, 5000U, 10000U, 20000U, 30000U, 50000U),
      ::testing::Values(1U, 50U, 100U, 200U, 500U, 1000U, 3000U, 6000U))
    );


// TODO(bmahler): Should also measure how expensive it is to
// add a framework after the slaves are added.
TEST_P(HierarchicalAllocator_BENCHMARK_Test, AddAndUpdateSlave)
{
  size_t slaveCount = std::tr1::get<0>(GetParam());
  size_t frameworkCount = std::tr1::get<1>(GetParam());

  vector<SlaveInfo> slaves;
  slaves.reserve(slaveCount);

  vector<FrameworkInfo> frameworks;
  frameworks.reserve(frameworkCount);

  for (unsigned i = 0; i < slaveCount; i++) {
    slaves.push_back(createSlaveInfo(
        "cpus:2;mem:1024;disk:4096;ports:[31000-32000]"));
  }

  for (unsigned i = 0; i < frameworkCount; i++) {
    frameworks.push_back(createFrameworkInfo(
        "*",
        {FrameworkInfo::Capability::REVOCABLE_RESOURCES}));
  }

  cout << "Using " << slaveCount << " agents"
       << " and " << frameworkCount << " frameworks" << endl;

  Clock::pause();

  atomic<size_t> offerCallbacks(0);

  auto offerCallback = [&offerCallbacks](
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources) {
    offerCallbacks++;
  };

  initialize(master::Flags(), offerCallback);

  Stopwatch watch;
  watch.start();

  foreach (const FrameworkInfo& framework, frameworks) {
    allocator->addFramework(framework.id(), framework, {});
  }

  // Wait for all the `addFramework` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << frameworkCount << " frameworks"
       << " in " << watch.elapsed() << endl;

  const Resources slaveResources = Resources::parse(
      "cpus:1;mem:128;disk:1024;"
      "ports:[31126-31510,31512-31623,31810-31852,31854-31964]").get();

  watch.start();

  // Add the slaves, use round-robin to choose which framework
  // to allocate a slice of the slave's resources to.
  for (unsigned i = 0; i < slaves.size(); i++) {
    hashmap<FrameworkID, Resources> used;

    used[frameworks[i % frameworkCount].id()] = slaveResources;

    allocator->addSlave(
        slaves[i].id(),
        slaves[i],
        None(),
        slaves[i].resources(),
        used);
  }

  // Wait for all the `addSlave` operations to be processed.
  Clock::settle();

  watch.stop();

  ASSERT_EQ(slaveCount, offerCallbacks.load());

  cout << "Added " << slaveCount << " agents"
       << " in " << watch.elapsed() << endl;

  // Oversubscribed resources on each slave.
  Resource oversubscribed = Resources::parse("cpus", "10", "*").get();
  oversubscribed.mutable_revocable();

  watch.start(); // Reset.

  foreach (const SlaveInfo& slave, slaves) {
    allocator->updateSlave(slave.id(), oversubscribed);
  }

  // Wait for all the `updateSlave` operations to be processed.
  Clock::settle();

  watch.stop();

  ASSERT_EQ(slaveCount * 2, offerCallbacks.load());

  cout << "Updated " << slaveCount << " agents in " << watch.elapsed() << endl;
}


// This benchmark simulates a number of frameworks that have a fixed amount of
// work to do. Once they have reached their targets, they start declining all
// subsequent offers.
TEST_P(HierarchicalAllocator_BENCHMARK_Test, DeclineOffers)
{
  size_t slaveCount = std::tr1::get<0>(GetParam());
  size_t frameworkCount = std::tr1::get<1>(GetParam());

  master::Flags flags;

  // Choose an interval longer than the time we expect a single cycle to take so
  // that we don't back up the process queue.
  flags.allocation_interval = Hours(1);

  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  struct OfferedResources {
    FrameworkID   frameworkId;
    SlaveID       slaveId;
    Resources     resources;
  };

  vector<OfferedResources> offers;

  auto offerCallback = [&offers](
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources_)
  {
    foreach (auto resources, resources_) {
      offers.push_back(
          OfferedResources{frameworkId, resources.first, resources.second});
    }
  };

  cout << "Using " << slaveCount << " agents and "
       << frameworkCount << " frameworks" << endl;

  vector<SlaveInfo> slaves;
  slaves.reserve(slaveCount);

  vector<FrameworkInfo> frameworks;
  frameworks.reserve(frameworkCount);

  initialize(flags, offerCallback);

  Stopwatch watch;
  watch.start();

  for (unsigned i = 0; i < frameworkCount; i++) {
    frameworks.push_back(createFrameworkInfo("*"));
    allocator->addFramework(frameworks[i].id(), frameworks[i], {});
  }

  // Wait for all the `addFramework` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << frameworkCount << " frameworks in "
       << watch.elapsed() << endl;

  Resources resources = Resources::parse(
      "cpus:16;mem:2014;disk:1024").get();

  Try<::mesos::Value::Ranges> ranges = fragment(createRange(31000, 32000), 16);
  ASSERT_SOME(ranges);
  ASSERT_EQ(16, ranges->range_size());

  resources += createPorts(ranges.get());

  watch.start();

  for (unsigned i = 0; i < slaveCount; i++) {
    slaves.push_back(createSlaveInfo(
        "cpus:24;mem:4096;disk:4096;ports:[31000-32000]"));

    // Add some used resources on each slave. Let's say there are 16 tasks, each
    // is allocated 1 cpu and a random port from the port range.
    hashmap<FrameworkID, Resources> used;
    used[frameworks[i % frameworkCount].id()] = resources;
    allocator->addSlave(
        slaves[i].id(), slaves[i], None(), slaves[i].resources(), used);
  }

  // Wait for all the `addSlave` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << slaveCount << " agents in "
       << watch.elapsed() << endl;

  // Loop enough times for all the frameworks to get offered all the resources.
  for (unsigned i = 0; i < frameworkCount * 2; i++) {
    // Permanently decline any offered resources.
    foreach (auto offer, offers) {
      Filters filters;

      filters.set_refuse_seconds(INT_MAX);
      allocator->recoverResources(
          offer.frameworkId, offer.slaveId, offer.resources, filters);
    }

    // Wait for the declined offers.
    Clock::settle();
    offers.clear();

    watch.start();

    // Advance the clock and trigger a background allocation cycle.
    Clock::advance(flags.allocation_interval);
    Clock::settle();

    watch.stop();

    cout << "round " << i
         << " allocate() took " << watch.elapsed()
         << " to make " << offers.size() << " offers"
         << endl;
  }

  Clock::resume();
}


// Returns the requested number of labels:
//   [{"<key>_1": "<value>_1"}, ..., {"<key>_<count>":"<value>_<count>"}]
static Labels createLabels(
    const string& key,
    const string& value,
    size_t count)
{
  Labels labels;

  for (size_t i = 0; i < count; i++) {
    const string index = stringify(i);
    labels.add_labels()->CopyFrom(createLabel(key + index, value + index));
  }

  return labels;
}


// TODO(neilc): Refactor to reduce code duplication with `DeclineOffers` test.
TEST_P(HierarchicalAllocator_BENCHMARK_Test, ResourceLabels)
{
  size_t slaveCount = std::tr1::get<0>(GetParam());
  size_t frameworkCount = std::tr1::get<1>(GetParam());

  master::Flags flags;

  // Choose an interval longer than the time we expect a single cycle to take so
  // that we don't back up the process queue.
  flags.allocation_interval = Hours(1);

  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  struct OfferedResources {
    FrameworkID   frameworkId;
    SlaveID       slaveId;
    Resources     resources;
  };

  vector<OfferedResources> offers;

  auto offerCallback = [&offers](
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources_)
  {
    foreach (auto resources, resources_) {
      offers.push_back(
          OfferedResources{frameworkId, resources.first, resources.second});
    }
  };

  cout << "Using " << slaveCount << " agents and "
       << frameworkCount << " frameworks" << endl;

  vector<SlaveInfo> slaves;
  slaves.reserve(slaveCount);

  vector<FrameworkInfo> frameworks;
  frameworks.reserve(frameworkCount);

  initialize(flags, offerCallback);

  Stopwatch watch;
  watch.start();

  for (unsigned i = 0; i < frameworkCount; i++) {
    frameworks.push_back(createFrameworkInfo("role1"));
    allocator->addFramework(frameworks[i].id(), frameworks[i], {});
  }

  // Wait for all the `addFramework` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << frameworkCount << " frameworks in "
       << watch.elapsed() << endl;

  // Create the used resources at each slave. We use three blocks of
  // resources: unreserved mem/disk/ports, and two different labeled
  // reservations with distinct labels. We choose the labels so that
  // the last label (in storage order) is different, which is the
  // worst-case for the equality operator. We also ensure that the
  // labels at any two nodes are distinct, which means they can't be
  // aggregated easily by the master/allocator.
  Resources resources = Resources::parse("mem:2014;disk:1024").get();

  Try<::mesos::Value::Ranges> ranges = fragment(createRange(31000, 32000), 16);
  ASSERT_SOME(ranges);
  ASSERT_EQ(16, ranges->range_size());

  resources += createPorts(ranges.get());

  watch.start();

  for (unsigned i = 0; i < slaveCount; i++) {
    slaves.push_back(createSlaveInfo(
        "cpus:24;mem:4096;disk:4096;ports:[31000-32000]"));

    Resources agentResources = resources;

    // We create reservations with 12 labels as we expect this is
    // more than most frameworks use. Note that only the 12th
    // label differs between the two sets of labels as this triggers
    // the pathological performance path in the Labels equality
    // operator.
    //
    // We add a unique id to each agent's reservation labels to
    // ensure that any aggregation across agents leads to
    // pathological performance (reservations with distinct labels
    // cannot be merged).
    //
    // TODO(neilc): Test with longer key / value lengths.
    Labels labels1 = createLabels("key", "value", 11);
    labels1.add_labels()->CopyFrom(
        createLabel("unique_key_1", "value_" + stringify(i)));

    Labels labels2 = createLabels("key", "value", 11);
    labels1.add_labels()->CopyFrom(
        createLabel("unique_key_2", "value_" + stringify(i)));

    Resources reserved1 =
      createReservedResource("cpus", "8", "role1",
                             createReservationInfo("principal1", labels1));
    Resources reserved2 =
      createReservedResource("cpus", "8", "role1",
                             createReservationInfo("principal1", labels2));

    agentResources += reserved1;
    agentResources += reserved2;

    // Add some used resources on each slave. Let's say there are 16 tasks, each
    // is allocated 1 cpu and a random port from the port range.
    hashmap<FrameworkID, Resources> used;
    used[frameworks[i % frameworkCount].id()] = agentResources;
    allocator->addSlave(
        slaves[i].id(), slaves[i], None(), slaves[i].resources(), used);
  }

  // Wait for all the `addSlave` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << slaveCount << " agents in "
       << watch.elapsed() << endl;

  // Loop enough times for all the frameworks to get offered all the resources.
  for (unsigned i = 0; i < frameworkCount * 2; i++) {
    // Permanently decline any offered resources.
    foreach (auto offer, offers) {
      Filters filters;

      filters.set_refuse_seconds(INT_MAX);
      allocator->recoverResources(
          offer.frameworkId, offer.slaveId, offer.resources, filters);
    }

    // Wait for the declined offers.
    Clock::settle();
    offers.clear();

    watch.start();

    // Advance the clock and trigger a background allocation cycle.
    Clock::advance(flags.allocation_interval);
    Clock::settle();

    watch.stop();

    cout << "round " << i
         << " allocate() took " << watch.elapsed()
         << " to make " << offers.size() << " offers"
         << endl;
  }

  Clock::resume();
}


// Measures the processing time required for the allocator metrics.
//
// TODO(bmahler): Add allocations to this benchmark.
TEST_P(HierarchicalAllocator_BENCHMARK_Test, Metrics)
{
  size_t slaveCount = std::tr1::get<0>(GetParam());
  size_t frameworkCount = std::tr1::get<1>(GetParam());

  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  initialize();

  Stopwatch watch;
  watch.start();

  for (size_t i = 0; i < frameworkCount; i++) {
    string role = stringify(i);
    allocator->setQuota(role, createQuota(role, "cpus:1;mem:512;disk:256"));
  }

  // Wait for all the `setQuota` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Set quota for " << frameworkCount << " roles in "
       << watch.elapsed() << endl;

  watch.start();

  for (size_t i = 0; i < frameworkCount; i++) {
    FrameworkInfo framework = createFrameworkInfo(stringify(i));
    allocator->addFramework(framework.id(), framework, {});
  }

  // Wait for all the `addFramework` operations to be processed.
  Clock::settle();

  watch.stop();

  cout << "Added " << frameworkCount << " frameworks in "
       << watch.elapsed() << endl;

  watch.start();

  for (size_t i = 0; i < slaveCount; i++) {
    SlaveInfo slave = createSlaveInfo("cpus:16;mem:2048;disk:1024");
    allocator->addSlave(slave.id(), slave, None(), slave.resources(), {});
  }

  // Wait for all the `addSlave` operations to complete.
  Clock::settle();

  watch.stop();

  cout << "Added " << slaveCount << " agents in "
       << watch.elapsed() << endl;

  // TODO(bmahler): Avoid timing the JSON parsing here.
  // Ideally we also avoid timing the HTTP layer.
  watch.start();
  JSON::Object metrics = Metrics();
  watch.stop();

  cout << "/metrics/snapshot took " << watch.elapsed()
       << " for " << slaveCount << " agents"
       << " and " << frameworkCount << " frameworks" << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
