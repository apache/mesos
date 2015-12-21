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
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/master/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/utils.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::MIN_CPUS;
using mesos::internal::master::MIN_MEM;

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::master::allocator::Allocator;

using mesos::quota::QuotaInfo;

using process::Clock;
using process::Future;

using std::atomic;
using std::cout;
using std::endl;
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

static Resource
makePortRanges(const ::mesos::Value::Range& bounds, unsigned numRanges)
{
  unsigned numPorts = bounds.end() - bounds.begin();
  unsigned step = numPorts / numRanges;
  ::mesos::Value::Ranges ranges;

  ranges.mutable_range()->Reserve(numRanges);

  for (unsigned i = 0; i < numRanges; ++i) {
    Value::Range *range = ranges.add_range();
    unsigned start = bounds.begin() + (i * step);
    unsigned end = start + 1;

    range->set_begin(start);
    range->set_end(end);
  }

  Value values;
  Resource resource;

  values.set_type(Value::RANGES);
  values.mutable_ranges()->CopyFrom(ranges);
  resource.set_type(Value::RANGES);
  resource.set_role("*");
  resource.set_name("ports");
  resource.mutable_ranges()->CopyFrom(values.ranges());

  return resource;
}

static ::mesos::Value::Range makeRange(unsigned begin, unsigned end)
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
        hashmap<string, double>());
  }

  SlaveInfo createSlaveInfo(const string& resources)
  {
    SlaveID slaveId;
    slaveId.set_value("slave" + stringify(nextSlaveId++));

    SlaveInfo slave;
    *(slave.mutable_resources()) = Resources::parse(resources).get();
    *(slave.mutable_id()) = slaveId;
    slave.set_hostname(slaveId.value());

    return slave;
  }

  FrameworkInfo createFrameworkInfo(const string& role)
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("framework" + stringify(nextFrameworkId++));
    frameworkInfo.mutable_id()->set_value(frameworkInfo.name());
    frameworkInfo.set_role(role);

    return frameworkInfo;
  }

  static QuotaInfo createQuotaInfo(const string& role, const string& resources)
  {
    QuotaInfo quota;
    quota.set_role(role);
    quota.mutable_guarantee()->CopyFrom(Resources::parse(resources).get());

    return quota;
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
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  // Total cluster resources will become cpus=2, mem=1024.
  SlaveInfo slave1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), EMPTY);

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 1

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  // Total cluster resources will become cpus=3, mem=1536:
  // role1 share = 0.66 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0
  SlaveInfo slave2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), EMPTY);

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
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), EMPTY);

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
  allocator->addFramework(
      framework3.id(), framework3, hashmap<SlaveID, Resources>());

  // Total cluster resources will become cpus=10, mem=7680:
  // role1 share = 0.2 (cpus=2, mem=1024)
  //   framework1 share = 1
  //   framework3 share = 0
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave4 = createSlaveInfo("cpus:4;mem:4096;disk:0");
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), EMPTY);

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
  allocator->addFramework(
      framework4.id(), framework4, hashmap<SlaveID, Resources>());

  // Total cluster resources will become cpus=11, mem=8192
  // role1 share = 0.63 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  //   framework4 share = 0
  // role2 share = 0.36 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave5 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave5.id(), slave5, None(), slave5.resources(), EMPTY);

  // Even though framework4 doesn't have any resources, role2 has a
  // lower share than role1, so framework2 receives slave5's resources.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave5.resources(), Resources::sum(allocation.get().resources));
}


// This test ensures that reserved resources do not affect the sharing
// across roles. However, reserved resources should be shared fairly
// *within* a role.
TEST_F(HierarchicalAllocatorTest, ReservedDRF)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave1 = createSlaveInfo(
      "cpus:1;mem:512;disk:0;"
      "cpus(role1):100;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), EMPTY);

  // framework1 will be offered all of the resources.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  // framework2 will be allocated the new resoures.
  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), EMPTY);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave2.resources(), Resources::sum(allocation.get().resources));

  // Now, even though framework1 has more resources allocated to
  // it than framework2, reserved resources are not considered for
  // fairness across roles! We expect framework1 to receive this
  // slave's resources, since it has fewer unreserved resources.
  SlaveInfo slave3 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), EMPTY);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // Now add another framework in role1. Since the reserved resources
  // should be allocated fairly between frameworks within a role, we
  // expect framework3 to receive the next allocation of role1
  // resources.
  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework3.id(), framework3, hashmap<SlaveID, Resources>());

  SlaveInfo slave4 = createSlaveInfo(
      "cpus(role1):2;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), EMPTY);

  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));
}


// This test ensures that agents which are scheduled for maintenance are
// properly sent inverse offers after they have accepted or reserved resources.
TEST_F(HierarchicalAllocatorTest, MaintenanceInverseOffers)
{
  // Pausing the clock is not necessary, but ensures that the test doesn't rely
  // on the periodic allocation in the allocator, which would slow down the
  // test.
  Clock::pause();

  initialize();

  // No initial resources.
  hashmap<FrameworkID, Resources> EMPTY;

  // Create an agent.
  SlaveInfo agent = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent.id(), agent, None(), agent.resources(), EMPTY);

  // This framework will be offered all of the resources.
  FrameworkInfo framework1 = createFrameworkInfo("*");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  // Check that the resources go to the framework.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
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
  EXPECT_EQ(framework1.id(), deallocation.get().frameworkId);
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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), EMPTY);

  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), EMPTY);

  // Once framework1 is added, an allocation will occur. Return the
  // resources so that we can test what happens when there are 2
  // frameworks and 2 slaves to consider during allocation.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

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
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  hashmap<FrameworkID, Allocation> frameworkAllocations;

  allocation = allocations.get();
  AWAIT_READY(allocation);
  frameworkAllocations[allocation.get().frameworkId] = allocation.get();

  allocation = allocations.get();
  AWAIT_READY(allocation);
  frameworkAllocations[allocation.get().frameworkId] = allocation.get();

  // Note that slave1 and slave2 have the same resources, we don't
  // care which framework received which slave.. only that they each
  // received one.
  ASSERT_TRUE(frameworkAllocations.contains(framework1.id()));
  ASSERT_EQ(1u, frameworkAllocations[framework1.id()].resources.size());
  EXPECT_EQ(slave1.resources(),
            Resources::sum(frameworkAllocations[framework1.id()].resources));

  ASSERT_TRUE(frameworkAllocations.contains(framework2.id()));
  ASSERT_EQ(1u, frameworkAllocations[framework1.id()].resources.size());
  EXPECT_EQ(slave2.resources(),
            Resources::sum(frameworkAllocations[framework1.id()].resources));
}


// This test ensures that frameworks that have the same share get an
// equal number of allocations over time (rather than the same
// framework getting all the allocations because it's name is
// lexicographically ordered first).
TEST_F(HierarchicalAllocatorTest, SameShareFairness)
{
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  FrameworkInfo framework1 = createFrameworkInfo("*");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  FrameworkInfo framework2 = createFrameworkInfo("*");
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  SlaveInfo slave = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave1 = createSlaveInfo(
      "cpus(role1):2;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), EMPTY);

  SlaveInfo slave2 = createSlaveInfo(
      "cpus(role2):2;mem(role2):1024;cpus:1;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), EMPTY);

  // This slave's resources should never be allocated, since there
  // is no framework for role3.
  SlaveInfo slave3 = createSlaveInfo(
      "cpus(role3):1;mem(role3):1024;disk(role3):0");
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), EMPTY);

  // framework1 should get all the resources from slave1, and the
  // unreserved resources from slave2.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

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
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo(
      "cpus(role1):1;mem(role1):200;"
      "cpus:1;mem:200;disk:0");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Initially, all the resources are allocated.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
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
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
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
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(1u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave.id()));
  EXPECT_EQ(unreserved, Resources::sum(allocation.get().resources));
}


TEST_F(HierarchicalAllocatorTest, Allocatable)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize();

  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

  hashmap<FrameworkID, Resources> EMPTY;

  // Not enough memory or cpu to be considered allocatable.
  SlaveInfo slave1 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS / 2) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave1.id(), slave1, None(), slave1.resources(), EMPTY);

  // Enough cpus to be considered allocatable.
  SlaveInfo slave2 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave2.id(), slave2, None(), slave2.resources(), EMPTY);

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
  allocator->addSlave(slave3.id(), slave3, None(), slave3.resources(), EMPTY);

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
  allocator->addSlave(slave4.id(), slave4, None(), slave4.resources(), EMPTY);

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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Initially, all the resources are allocated.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

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
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Expect to receive the all of the available resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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
  // Pause clock to disable periodic allocation.
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Add a framework that can accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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
  // Pause clock to disable periodic allocation.
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Add a framework that does *not* accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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
  // Pause clock to disable periodic allocation.
  Clock::pause();

  initialize();

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:100;mem:100;disk:100");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  // Add a framework that can accept revocable resources.
  FrameworkInfo framework = createFrameworkInfo("role1");
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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
  whitelist.insert("dummy-slave");

  allocator->updateWhitelist(whitelist);

  hashmap<FrameworkID, Resources> EMPTY;

  SlaveInfo slave = createSlaveInfo("cpus:2;mem:1024");
  allocator->addSlave(slave.id(), slave, None(), slave.resources(), EMPTY);

  FrameworkInfo framework = createFrameworkInfo("*");
  allocator->addFramework(
      framework.id(), framework, hashmap<SlaveID, Resources>());

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
// the quota'ed role declines offers, some resources are kept aside for
// the role, so that a greedy framework from a non-quota'ed role cannot
// eat up all free resources.
TEST_F(HierarchicalAllocatorTest, QuotaProvidesQuarantee)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  hashmap<FrameworkID, Resources> EMPTY;

  initialize();

  // Create `framework1` and set quota for its role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota1);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  // Process all triggered allocation events.
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), EMPTY);

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
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), EMPTY);

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

  // Now `framework1` declines the second offer and sets a filter for 5
  // seconds. The declined resources should not be offered to `framework2`
  // because by doing so they may not be available to `framework1` when
  // the filter expires.
  Filters filter5s;
  filter5s.set_refuse_seconds(5.);
  allocator->recoverResources(
      framework1.id(),
      agent2.id(),
      allocation.get().resources.get(agent2.id()).get(),
      filter5s);

  // Total cluster resources: cpus=1, mem=512.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Assuming the default batch allocation interval is less than 5 seconds,
  // all periodic allocations that happen while the refuse filter is active
  // should yield no new allocations.
  ASSERT_LT(flags.allocation_interval.secs(), filter5s.refuse_seconds());
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  // TODO(alexr): There is currently no way to check the absence of
  // allocations. The `process::Queue` class does not support any size
  // checking methods. Consider adding `process::Queue::empty()` or
  // refactor the test harness so that we can reason about whether the
  // Hierarchical allocator has assigned expected allocations or not.
  // NOTE: It is hard to capture the absense of an allocation in a
  // general case, because an allocator may be complex enough to postpone
  // decisions beyond its allocation cycle.

  // Now advance the clock to make sure the filter is expired. The next
  // and only allocation should be the declined resources offered to the
  // quota'ed role.
  Clock::advance(Duration::create(filter5s.refuse_seconds()).get());
  Clock::settle();

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


// If quota is removed, fair sharing should be restored in the cluster
// after sufficient number of tasks finish.
TEST_F(HierarchicalAllocatorTest, RemoveQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // We start with the following cluster setup.
  // Total cluster resources (2 identical agents): cpus=2, mem=1024.
  // QUOTA_ROLE share = 1 (cpus=2, mem=1024) [quota: cpus=2, mem=1024]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Create framework and agent descriptions.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");

  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:2;mem:1024");

  // Notify allocator of agents, frameworks, quota and current allocations.
  allocator->setQuota(QUOTA_ROLE, quota1);

  // NOTE: We do not report about allocated resources to `framework1`
  // here to avoid double accounting.
  allocator->addFramework(
      framework1.id(),
      framework1,
      hashmap<SlaveID, Resources>());

  allocator->addFramework(
      framework2.id(),
      framework2,
      hashmap<SlaveID, Resources>());

  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      {std::make_pair(framework1.id(), agent2.resources())});

  // All cluster resources are now being used by `framework1` as part of
  // its role quota, no further allocations are expected. However, once the
  // quota is removed, quota guarantee does not apply any more and released
  // resources should be offered to `framework2` to restore fairness.

  allocator->removeQuota(QUOTA_ROLE);

  // Process all triggered allocation events.
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  allocator->recoverResources(
      framework1.id(),
      agent1.id(),
      agent1.resources(),
      None());

  // Trigger the next periodic allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // Total cluster resources: cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework2 share = 1
}


// If a quota'ed role contains multiple frameworks, the resources should
// be distributed fairly between them. However, inside the quota'ed role,
// if one framework declines resources, there is no guarantee the other
// framework in the same role does not consume all role's quota.
TEST_F(HierarchicalAllocatorTest, MultipleFrameworksInRoleWithQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  hashmap<FrameworkID, Resources> EMPTY;

  initialize();

  // Create `framework1a` and set quota for its role.
  FrameworkInfo framework1a = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(
      framework1a.id(), framework1a, hashmap<SlaveID, Resources>());

  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:4;mem:2048");
  allocator->setQuota(QUOTA_ROLE, quota1);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  // Process all triggered allocation events.
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), EMPTY);

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
  allocator->addFramework(
      framework1b.id(), framework1b, hashmap<SlaveID, Resources>());

  SlaveInfo agent2 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), EMPTY);

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
  allocator->addSlave(agent3.id(), agent3, None(), agent3.resources(), EMPTY);

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

  // Trigger the next periodic allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

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
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  hashmap<FrameworkID, Resources> EMPTY;

  initialize();

  // Create `framework1` and set quota for its role.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  // Set quota to be less than the agent resources.
  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:0.5;mem:200");
  allocator->setQuota(QUOTA_ROLE, quota1);

  // Create `framework2` in a non-quota'ed role.
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(
      framework2.id(), framework2, hashmap<SlaveID, Resources>());

  // Process all triggered allocation events.
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), EMPTY);

  // `framework1` will be offered all of `agent1`'s resources because
  // it is the only framework in the only role with unsatisfied quota
  // and the allocator performs coarse-grained allocation.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));
  EXPECT_TRUE(Resources(agent1.resources()).contains(quota1.guarantee()));

  // Total cluster resources: cpus=1, mem=512.
  // QUOTA_ROLE share = 1 (cpus=1, mem=512) [quota: cpus=0.5, mem=200]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0
}


// This test verifies, that the free pool (what is left after all quotas
// are satisfied) is allocated according to the DRF algorithm.
TEST_F(HierarchicalAllocatorTest, DRFWithQuota)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // We start with the following cluster setup.
  // Total cluster resources (1 agent): cpus=1, mem=512.
  // QUOTA_ROLE share = 0.25 (cpus=0.25, mem=128) [quota: cpus=0.25, mem=128]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Create framework and agent descriptions.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");

  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:0.25;mem:128");

  // Notify allocator of agents, frameworks, quota and current allocations.
  allocator->setQuota(QUOTA_ROLE, quota1);

  // NOTE: We do not report about allocated resources to `framework1`
  // here to avoid double accounting.
  allocator->addFramework(
      framework1.id(),
      framework1,
      hashmap<SlaveID, Resources>());

  allocator->addFramework(
      framework2.id(),
      framework2,
      hashmap<SlaveID, Resources>());

  // Process all triggered allocation events.
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), Resources(quota1.guarantee()))});

  // Some resources on `agent1` are now being used by `framework1` as part
  // of its role quota. All quotas are satisfied, all available resources
  // should be allocated according to fair shares of roles and frameworks.

  // `framework2` will be offered all of `agent1`'s resources because its
  // share is 0.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources() - Resources(quota1.guarantee()),
            Resources::sum(allocation.get().resources));

  // Total cluster resources (1 agent): cpus=1, mem=512.
  // QUOTA_ROLE share = 0.25 (cpus=0.25, mem=128) [quota: cpus=0.25, mem=128]
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0.75 (cpus=0.75, mem=384)
  //   framework2 share = 0

  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      hashmap<FrameworkID, Resources>());

  // `framework1` will be offered all of `agent2`'s resources
  // (coarse-grained allocation) because its share is less than
  // `framework2`'s share.
  allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
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
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  initialize();

  // We want to start with the following cluster setup.
  // Total cluster resources (2 identical agents): cpus=2, mem=1024.
  // QUOTA_ROLE share = 0.5 (cpus=1, mem=512)
  //   framework1 share = 1
  // NO_QUOTA_ROLE share = 0
  //   framework2 share = 0

  // Create framework and agent descriptions.
  FrameworkInfo framework1 = createFrameworkInfo(QUOTA_ROLE);
  FrameworkInfo framework2 = createFrameworkInfo(NO_QUOTA_ROLE);

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");

  // Notify allocator of agents, frameworks, and current allocations.
  // NOTE: We do not report about allocated resources to `framework1`
  // here to avoid double accounting.
  allocator->addFramework(
      framework1.id(),
      framework1,
      hashmap<SlaveID, Resources>());

  allocator->addFramework(
      framework2.id(),
      framework2,
      hashmap<SlaveID, Resources>());

  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      {std::make_pair(framework1.id(), agent1.resources())});

  // Process all triggered allocation events.
  // NOTE: No allocations happen because all resources are already allocated.
  Clock::settle();

  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      hashmap<FrameworkID, Resources>());

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

  // Trigger the next periodic allocation.
  Clock::advance(flags.allocation_interval);
  Clock::settle();

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
  QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota1);

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


// This test checks that quota is respected even for roles that don't
// have any frameworks currently registered.
TEST_F(HierarchicalAllocatorTest, QuotaAbsentFramework)
{
  Clock::pause();

  const string QUOTA_ROLE{"quota-role"};
  const string NO_QUOTA_ROLE{"no-quota-role"};

  hashmap<FrameworkID, Resources> EMPTY;

  initialize();

  SlaveInfo agent1 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");

  allocator->addSlave(agent1.id(), agent1, None(), agent1.resources(), EMPTY);
  allocator->addSlave(agent2.id(), agent2, None(), agent2.resources(), EMPTY);

  // Set quota for the quota'ed role.
  const QuotaInfo quota1 = createQuotaInfo(QUOTA_ROLE, "cpus:2;mem:1024");
  allocator->setQuota(QUOTA_ROLE, quota1);

  // Add `framework1` in the non-quota'ed role.
  FrameworkInfo framework1 = createFrameworkInfo(NO_QUOTA_ROLE);
  allocator->addFramework(
      framework1.id(), framework1, hashmap<SlaveID, Resources>());

  // `framework1` can only be allocated resources on `agent2`. This
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
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent2.resources(), Resources::sum(allocation.get().resources));
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
      ::testing::Values(1U, 50U, 100U, 200U, 500U, 1000U))
    );


// TODO(bmahler): Should also measure how expensive it is to
// add a framework after the slaves are added.
TEST_P(HierarchicalAllocator_BENCHMARK_Test, AddAndUpdateSlave)
{
  size_t slaveCount = std::tr1::get<0>(GetParam());
  size_t frameworkCount = std::tr1::get<1>(GetParam());

  vector<SlaveInfo> slaves;
  vector<FrameworkInfo> frameworks;

  for (unsigned i = 0; i < slaveCount; i++) {
    slaves.push_back(createSlaveInfo(
        "cpus:2;mem:1024;disk:4096;ports:[31000-32000]"));
  }

  for (unsigned i = 0; i < frameworkCount; ++i) {
    frameworks.push_back(createFrameworkInfo("*"));
    frameworks.back().add_capabilities()->set_type(
        FrameworkInfo::Capability::REVOCABLE_RESOURCES);
  }

  cout << "Using " << slaveCount << " slaves"
       << " and " << frameworkCount << " frameworks" << endl;

  Clock::pause();

  // Number of allocations. This is used to determine
  // the termination condition.
  atomic<size_t> finished(0);

  auto offerCallback = [&finished](
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources) {
    finished++;
  };

  initialize(master::Flags(), offerCallback);

  Stopwatch watch;
  watch.start();

  foreach (const FrameworkInfo& framework, frameworks) {
    allocator->addFramework(framework.id(), framework, {});
  }

  cout << "Added " << frameworkCount << " frameworks"
       << " in " << watch.elapsed() << endl;

  watch.start();

  // Add the slaves, use round-robin to choose which framework
  // to allocate a slice of the slave's resources to.
  for (unsigned i = 0; i < slaves.size(); ++i) {
    hashmap<FrameworkID, Resources> used;

    used[frameworks[i % frameworkCount].id()] = Resources::parse(
        "cpus:1;mem:128;disk:1024;"
        "ports:[31126-31510,31512-31623,31810-31852,31854-31964]").get();

    allocator->addSlave(
        slaves[i].id(),
        slaves[i],
        None(),
        slaves[i].resources(),
        used);
  }

  // Wait for all the 'addSlave' operations to be processed.
  while (finished.load() != slaveCount) {
    os::sleep(Milliseconds(10));
  }

  cout << "Added " << slaveCount << " slaves"
       << " in " << watch.elapsed() << endl;

  // Oversubscribed resources on each slave.
  Resource oversubscribed = Resources::parse("cpus", "10", "*").get();
  oversubscribed.mutable_revocable();

  watch.start(); // Reset.

  foreach (const SlaveInfo& slave, slaves) {
    allocator->updateSlave(slave.id(), oversubscribed);
  }

  // Wait for all the 'updateSlave' operations to be processed.
  while (finished.load() != 2 * slaveCount) {
    os::sleep(Milliseconds(10));
  }

  cout << "Updated " << slaveCount << " slaves in " << watch.elapsed() << endl;
}


// This benchmark simulates a number of frameworks that have a fixed amount of
// work to do. Once they have reached their targets, they start declining all
// subsequent offers.
TEST_F(HierarchicalAllocator_BENCHMARK_Test, DeclineOffers)
{
  unsigned frameworkCount = 200;
  unsigned slaveCount = 2000;
  master::Flags flags;

  FLAGS_v = 5;
  __sync_synchronize(); // Ensure 'FLAGS_v' visible in other threads.

  // Choose an interval longer than the time we expect a single cycle to take so
  // that we don't back up the process queue.
  flags.allocation_interval = Hours(1);

  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  // Number of allocations. This is used to determine the termination
  // condition.
  atomic<size_t> offerCount(0);

  struct OfferedResources {
    FrameworkID   frameworkId;
    SlaveID       slaveId;
    Resources     resources;
  };

  std::vector<OfferedResources> offers;

  auto offerCallback = [&offerCount, &offers](
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources_)
  {
    for (auto resources : resources_) {
      offers.push_back(
          OfferedResources{frameworkId, resources.first, resources.second});
    }

    offerCount++;
  };

  vector<SlaveInfo> slaves;
  vector<FrameworkInfo> frameworks;

  cout << "Using " << slaveCount << " slaves and "
       << frameworkCount << " frameworks" << endl;

  slaves.reserve(slaveCount);
  frameworks.reserve(frameworkCount);

  initialize(flags, offerCallback);

  for (unsigned i = 0; i < frameworkCount; ++i) {
    frameworks.push_back(createFrameworkInfo("*"));
    allocator->addFramework(frameworks[i].id(), frameworks[i], {});
  }

  Resources resources = Resources::parse(
      "cpus:16;mem:2014;disk:1024;").get();

  Resources ports = makePortRanges(makeRange(31000, 32000), 16);

  resources += ports;

  for (unsigned i = 0; i < slaveCount; ++i) {
    slaves.push_back(createSlaveInfo(
        "cpus:24;mem:4096;disk:4096;ports:[31000-32000]"));

    // Add some used resources on each slave. Let's say there are 16 tasks, each
    // is allocated 1 cpu and a random port from the port range.
    hashmap<FrameworkID, Resources> used;
    used[frameworks[i % frameworkCount].id()] = resources;
    allocator->addSlave(
        slaves[i].id(), slaves[i], None(), slaves[i].resources(), used);
  }

  // Wait for all the 'addSlave' operations to be processed.
  Clock::settle();

  // Loop enough times for all the frameworks to get offerred all the resources.
  for (unsigned count = 0; count < frameworkCount * 2; ++count) {
    // Permanently decline any offered resources.
    for (auto offer : offers) {
      Filters filters;

      filters.set_refuse_seconds(INT_MAX);
      allocator->recoverResources(
          offer.frameworkId, offer.slaveId, offer.resources, filters);
    }

    // Wait for the declined offers.
    Clock::settle();
    offers.clear();
    offerCount = 0;

    {
      Stopwatch watch;

      watch.start();

      // Advance the clock and trigger a background allocation cycle.
      Clock::advance(flags.allocation_interval);
      Clock::settle();

      cout << "round " << count
           << " allocate took " << watch.elapsed()
           << " to make " << offerCount.load() << " offers"
           << endl;
    }
  }

  Clock::resume();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
