/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>
#include <iostream>
#include <string>
#include <queue>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/master/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/shared.hpp>
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

using mesos::master::allocator::Allocator;
using mesos::master::RoleInfo;
using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using process::Clock;
using process::Future;
using process::Shared;

using std::atomic;
using std::cout;
using std::endl;
using std::queue;
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
      const vector<string>& _roles,
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

    // NOTE: The master always adds this default role.
    RoleInfo info;
    info.set_name("*");
    roles["*"] = info;

    foreach (const string& role, _roles) {
      info.set_name(role);
      roles[role] = info;
    }

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
        roles);
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

  hashmap<string, RoleInfo> roles;

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

  initialize(vector<string>{"role1", "role2"});

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

  initialize(vector<string>{"role1", "role2"});

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

  hashmap<FrameworkID, Resources> EMPTY;

  initialize(vector<string>{ROLE});

  FrameworkInfo framework1 = createFrameworkInfo(ROLE);

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");

  allocator->addFramework(
      framework1.id(),
      framework1,
      hashmap<SlaveID, Resources>());

  allocator->addSlave(
      agent1.id(),
      agent1,
      None(),
      agent1.resources(),
      EMPTY);

  // `framework1` will be offered all of `agent1` resources
  // because it is the only framework in the cluster.
  Future<Allocation> allocation = allocations.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));

  // Now `framework1` declines the offer and sets a filter
  // with the duration greater than the allocation interval.
  Duration filterTimeout = flags.allocation_interval * 2;
  Filters offerFilter;
  offerFilter.set_refuse_seconds(filterTimeout.secs());

  allocator->recoverResources(
      framework1.id(),
      agent1.id(),
      allocation.get().resources.get(agent1.id()).get(),
      offerFilter);

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
  Clock::settle();

  // The next batch allocation should offer resources to `framework1`.
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(agent1.resources(), Resources::sum(allocation.get().resources));
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

  hashmap<FrameworkID, Resources> EMPTY;

  // Explicitly set the allocation interval to make sure
  // it is greater than the offer filter timeout.
  master::Flags flags_;
  flags_.allocation_interval = Minutes(1);

  initialize(vector<string>{ROLE}, flags_);

  // We start with the following cluster setup.
  // Total cluster resources (1 agent): cpus=1, mem=512.
  // ROLE1 share = 1 (cpus=1, mem=512)
  //   framework1 share = 1 (cpus=1, mem=512)
  //   framework2 share = 0

  FrameworkInfo framework1 = createFrameworkInfo(ROLE);
  FrameworkInfo framework2 = createFrameworkInfo(ROLE);

  SlaveInfo agent1 = createSlaveInfo("cpus:1;mem:512;disk:0");

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
  // NOTE: No allocations happen because there are no resources to allocate.
  Clock::settle();

  // Add one more agent with some free resources.
  SlaveInfo agent2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(
      agent2.id(),
      agent2,
      None(),
      agent2.resources(),
      EMPTY);

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
  Clock::settle();

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
  // Pausing the clock is not necessary, but ensures that the test doesn't rely
  // on the periodic allocation in the allocator, which would slow down the
  // test.
  Clock::pause();

  initialize(vector<string>{});

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

  initialize(vector<string>{"role1", "role2"});

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

  initialize(vector<string>{});

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

  initialize(vector<string>{"role1", "role2", "role3"});

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

  initialize(vector<string>{"role1"});

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

  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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
  initialize(vector<string>{"role1"});

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

  initialize(vector<string>{"role1"});

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


class HierarchicalAllocator_BENCHMARK_Test
  : public HierarchicalAllocatorTestBase,
    public WithParamInterface<std::tr1::tuple<size_t, size_t>>
{};


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

  initialize({}, master::Flags(), offerCallback);

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
