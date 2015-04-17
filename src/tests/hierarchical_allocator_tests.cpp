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

#include <gmock/gmock.h>

#include <string>
#include <queue>
#include <vector>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/shared.hpp>
#include <process/queue.hpp>

#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/utils.hpp>

#include "master/constants.hpp"
#include "master/flags.hpp"

#include "master/allocator/allocator.hpp"
#include "master/allocator/mesos/hierarchical.hpp"

using mesos::internal::master::MIN_CPUS;
using mesos::internal::master::MIN_MEM;

using mesos::internal::master::allocator::Allocator;
using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using process::Clock;
using process::Future;
using process::Shared;

using std::queue;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


struct Allocation
{
  FrameworkID frameworkId;
  hashmap<SlaveID, Resources> resources;
};


class HierarchicalAllocatorTest : public ::testing::Test
{
protected:
  HierarchicalAllocatorTest()
    : allocator(new HierarchicalDRFAllocator),
      nextSlaveId(1),
      nextFrameworkId(1) {}

  ~HierarchicalAllocatorTest()
  {
    delete allocator;
  }

  void initialize(
      const vector<string>& _roles,
      const master::Flags& _flags = master::Flags())
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

    allocator->initialize(
        flags,
        lambda::bind(&put, &queue, lambda::_1, lambda::_2),
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

private:
  static void put(
      process::Queue<Allocation>* queue,
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, Resources>& resources)
  {
    Allocation allocation;
    allocation.frameworkId = frameworkId;
    allocation.resources = resources;

    queue->put(allocation);
  }

protected:
  master::Flags flags;

  Allocator* allocator;

  process::Queue<Allocation> queue;

  hashmap<string, RoleInfo> roles;

private:
  int nextSlaveId;
  int nextFrameworkId;
};


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
  allocator->addSlave(slave1.id(), slave1, slave1.resources(), EMPTY);

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, Resources());

  Future<Allocation> allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 1 (cpus=2, mem=1024)
  //   framework1 share = 1

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, Resources());

  // Total cluster resources will become cpus=3, mem=1536:
  // role1 share = 0.66 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0
  SlaveInfo slave2 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, slave2.resources(), EMPTY);

  // framework2 will be offered all of slave2's resources since role2
  // has the lowest user share, and framework2 is its only framework.
  allocation = queue.get();
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
  allocator->addSlave(slave3.id(), slave3, slave3.resources(), EMPTY);

  // framework2 will be offered all of slave3's resources since role2
  // has the lowest share.
  allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0.71 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(framework3.id(), framework3, Resources());

  // Total cluster resources will become cpus=10, mem=7680:
  // role1 share = 0.2 (cpus=2, mem=1024)
  //   framework1 share = 1
  //   framework3 share = 0
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave4 = createSlaveInfo("cpus:4;mem:4096;disk:0");
  allocator->addSlave(slave4.id(), slave4, slave4.resources(), EMPTY);

  // framework3 will be offered all of slave4's resources since role1
  // has the lowest user share, and framework3 has the lowest share of
  // role1's frameworks.
  allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));

  // role1 share = 0.67 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  // role2 share = 0.4 (cpus=4, mem=2560)
  //   framework2 share = 1

  FrameworkInfo framework4 = createFrameworkInfo("role1");
  allocator->addFramework(framework4.id(), framework4, Resources());

  // Total cluster resources will become cpus=11, mem=8192
  // role1 share = 0.63 (cpus=6, mem=5120)
  //   framework1 share = 0.33 (cpus=2, mem=1024)
  //   framework3 share = 0.8 (cpus=4, mem=4096)
  //   framework4 share = 0
  // role2 share = 0.36 (cpus=4, mem=2560)
  //   framework2 share = 1
  SlaveInfo slave5 = createSlaveInfo("cpus:1;mem:512;disk:0");
  allocator->addSlave(slave5.id(), slave5, slave5.resources(), EMPTY);

  // Even though framework4 doesn't have any resources, role2 has a
  // lower share than role1, so framework2 receives slave5's resources.
  allocation = queue.get();
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
  allocator->addSlave(slave1.id(), slave1, slave1.resources(), EMPTY);

  // framework1 will be offered all of the resources.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, Resources());

  Future<Allocation> allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave1.resources(), Resources::sum(allocation.get().resources));

  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, Resources());

  // framework2 will be allocated the new resoures.
  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave2.id(), slave2, slave2.resources(), EMPTY);

  allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework2.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave2.resources(), Resources::sum(allocation.get().resources));

  // Now, even though framework1 has more resources allocated to
  // it than framework2, reserved resources are not considered for
  // fairness across roles! We expect framework1 to receive this
  // slave's resources, since it has fewer unreserved resources.
  SlaveInfo slave3 = createSlaveInfo("cpus:2;mem:512;disk:0");
  allocator->addSlave(slave3.id(), slave3, slave3.resources(), EMPTY);

  allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave3.resources(), Resources::sum(allocation.get().resources));

  // Now add another framework in role1. Since the reserved resources
  // should be allocated fairly between frameworks within a role, we
  // expect framework3 to receive the next allocation of role1
  // resources.
  FrameworkInfo framework3 = createFrameworkInfo("role1");
  allocator->addFramework(framework3.id(), framework3, Resources());

  SlaveInfo slave4 = createSlaveInfo(
      "cpus(role1):2;mem(role1):1024;disk(role1):0");
  allocator->addSlave(slave4.id(), slave4, slave4.resources(), EMPTY);

  allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework3.id(), allocation.get().frameworkId);
  EXPECT_EQ(slave4.resources(), Resources::sum(allocation.get().resources));
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
  allocator->addSlave(slave1.id(), slave1, slave1.resources(), EMPTY);

  SlaveInfo slave2 = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, slave2.resources(), EMPTY);

  // Once framework1 is added, an allocation will occur. Return the
  // resources so that we can test what happens when there are 2
  // frameworks and 2 slaves to consider during allocation.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, Resources());

  Future<Allocation> allocation = queue.get();
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
  allocator->addFramework(framework2.id(), framework2, Resources());

  hashmap<FrameworkID, Allocation> allocations;

  allocation = queue.get();
  AWAIT_READY(allocation);
  allocations[allocation.get().frameworkId] = allocation.get();

  allocation = queue.get();
  AWAIT_READY(allocation);
  allocations[allocation.get().frameworkId] = allocation.get();

  // Note that slave1 and slave2 have the same resources, we don't
  // care which framework received which slave.. only that they each
  // received one.
  ASSERT_TRUE(allocations.contains(framework1.id()));
  ASSERT_EQ(1u, allocations[framework1.id()].resources.size());
  EXPECT_EQ(slave1.resources(),
            Resources::sum(allocations[framework1.id()].resources));

  ASSERT_TRUE(allocations.contains(framework2.id()));
  ASSERT_EQ(1u, allocations[framework1.id()].resources.size());
  EXPECT_EQ(slave2.resources(),
            Resources::sum(allocations[framework1.id()].resources));
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
  allocator->addFramework(framework1.id(), framework1, Resources());

  FrameworkInfo framework2 = createFrameworkInfo("*");
  allocator->addFramework(framework2.id(), framework2, Resources());

  SlaveInfo slave = createSlaveInfo("cpus:2;mem:1024;disk:0");
  allocator->addSlave(slave.id(), slave, slave.resources(), EMPTY);

  // Ensure that the slave's resources are alternated between both
  // frameworks.
  hashmap<FrameworkID, size_t> counts;

  for (int i = 0; i < 10; i++) {
    Future<Allocation> allocation = queue.get();
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
  allocator->addSlave(slave1.id(), slave1, slave1.resources(), EMPTY);

  SlaveInfo slave2 = createSlaveInfo(
      "cpus(role2):2;mem(role2):1024;cpus:1;mem:1024;disk:0");
  allocator->addSlave(slave2.id(), slave2, slave2.resources(), EMPTY);

  // This slave's resources should never be allocated, since there
  // is no framework for role3.
  SlaveInfo slave3 = createSlaveInfo(
      "cpus(role3):1;mem(role3):1024;disk(role3):0");
  allocator->addSlave(slave3.id(), slave3, slave3.resources(), EMPTY);

  // framework1 should get all the resources from slave1, and the
  // unreserved resources from slave2.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, Resources());

  Future<Allocation> allocation = queue.get();
  AWAIT_READY(allocation);
  EXPECT_EQ(framework1.id(), allocation.get().frameworkId);
  EXPECT_EQ(2u, allocation.get().resources.size());
  EXPECT_TRUE(allocation.get().resources.contains(slave1.id()));
  EXPECT_TRUE(allocation.get().resources.contains(slave2.id()));
  EXPECT_EQ(slave1.resources() + Resources(slave2.resources()).unreserved(),
            Resources::sum(allocation.get().resources));

  // framework2 should get all of its reserved resources on slave2.
  FrameworkInfo framework2 = createFrameworkInfo("role2");
  allocator->addFramework(framework2.id(), framework2, Resources());

  allocation = queue.get();
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
  allocator->addSlave(slave.id(), slave, slave.resources(), EMPTY);

  // Initially, all the resources are allocated.
  FrameworkInfo framework1 = createFrameworkInfo("role1");
  allocator->addFramework(framework1.id(), framework1, Resources());

  Future<Allocation> allocation = queue.get();
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

  allocation = queue.get();
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

  allocation = queue.get();
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
  allocator->addFramework(framework.id(), framework, Resources());

  hashmap<FrameworkID, Resources> EMPTY;

  // Not enough memory or cpu to be considered allocatable.
  SlaveInfo slave1 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS / 2) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave1.id(), slave1, slave1.resources(), EMPTY);

  // Enough cpus to be considered allocatable.
  SlaveInfo slave2 = createSlaveInfo(
      "cpus:" + stringify(MIN_CPUS) + ";"
      "mem:" + stringify((MIN_MEM / 2).megabytes()) + ";"
      "disk:128");
  allocator->addSlave(slave2.id(), slave2, slave2.resources(), EMPTY);

  Future<Allocation> allocation = queue.get();
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
  allocator->addSlave(slave3.id(), slave3, slave3.resources(), EMPTY);

  allocation = queue.get();
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
  allocator->addSlave(slave4.id(), slave4, slave4.resources(), EMPTY);

  allocation = queue.get();
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
  allocator->addSlave(slave.id(), slave, slave.resources(), EMPTY);

  // Initially, all the resources are allocated.
  FrameworkInfo framework = createFrameworkInfo("role1");
  allocator->addFramework(framework.id(), framework, Resources());

  Future<Allocation> allocation = queue.get();
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

  allocation = queue.get();
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
  allocator->addSlave(slave.id(), slave, slave.resources(), EMPTY);

  FrameworkInfo framework = createFrameworkInfo("*");
  allocator->addFramework(framework.id(), framework, Resources());

  Future<Allocation> allocation = queue.get();

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
