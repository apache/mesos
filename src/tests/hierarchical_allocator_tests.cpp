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
#include <process/queue.hpp>

#include <stout/hashmap.hpp>

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/hierarchical_allocator_process.hpp"

using namespace mesos;
using namespace mesos::internal;

using mesos::internal::master::allocator::Allocator;
using mesos::internal::master::allocator::AllocatorProcess;
using mesos::internal::master::allocator::HierarchicalDRFAllocatorProcess;

using process::Clock;
using process::Future;

using std::queue;
using std::string;
using std::vector;


struct Allocation
{
  FrameworkID frameworkId;
  hashmap<SlaveID, Resources> resources;
};


class HierarchicalAllocatorTest : public ::testing::Test
{
protected:
  HierarchicalAllocatorTest()
    : allocatorProcess(new HierarchicalDRFAllocatorProcess()),
      allocator(new Allocator(allocatorProcess)),
      nextSlaveId(1),
      nextFrameworkId(1) {}

  ~HierarchicalAllocatorTest()
  {
    delete allocator;
    delete allocatorProcess;
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

  AllocatorProcess* allocatorProcess;
  Allocator* allocator;

  process::Queue<Allocation> queue;

  hashmap<string, RoleInfo> roles;

private:
  int nextSlaveId;
  int nextFrameworkId;
};


template <typename Iterable>
Resources sum(const Iterable& iterable)
{
  Resources total;
  foreach (const Resources& resources, iterable) {
    total += resources;
  }
  return total;
}


// Checks that the DRF allocator implements the DRF algorithm
// correctly. The test accomplishes this by adding frameworks and
// slaves one at a time to the allocator, making sure that each time
// a new slave is added all of its resources are offered to whichever
// framework currently has the smallest share. Checking for proper DRF
// logic when resources are returned, frameworks exit, etc. is handled
// by SorterTest.DRFSorter.
TEST_F(HierarchicalAllocatorTest, DRF)
{
  // Pausing the clock is not necessary, but ensures that the test
  // doesn't rely on the periodic allocation in the allocator, which
  // would slow down the test.
  Clock::pause();

  initialize({"role1", "role2"});

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
  EXPECT_EQ(slave1.resources(), sum(allocation.get().resources.values()));

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
  EXPECT_EQ(slave2.resources(), sum(allocation.get().resources.values()));

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
  EXPECT_EQ(slave3.resources(), sum(allocation.get().resources.values()));

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
  EXPECT_EQ(slave4.resources(), sum(allocation.get().resources.values()));

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
  EXPECT_EQ(slave5.resources(), sum(allocation.get().resources.values()));
}


// This test ensures that allocation is done per slave. This is done
// by having 2 slaves and 2 frameworks and making sure each framework
// gets only one slave's resources during an allocation.
TEST_F(HierarchicalAllocatorTest, CoarseGrained)
{
  // Pausing the clock ensures that the batch allocation does not
  // influence this test.
  Clock::pause();

  initialize({"role1", "role2"});

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
            sum(allocation.get().resources.values()));

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
            sum(allocations[framework1.id()].resources.values()));

  ASSERT_TRUE(allocations.contains(framework2.id()));
  ASSERT_EQ(1u, allocations[framework1.id()].resources.size());
  EXPECT_EQ(slave2.resources(),
            sum(allocations[framework1.id()].resources.values()));
}
