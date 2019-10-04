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

#include <mesos/allocator/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/stopwatch.hpp>

#include "master/constants.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::MIN_CPUS;
using mesos::internal::master::MIN_MEM;

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::slave::AGENT_CAPABILITIES;

using mesos::allocator::Allocator;
using mesos::allocator::Options;

using process::Clock;
using process::Future;

using std::cout;
using std::endl;
using std::make_shared;
using std::ostream;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// TODO(kapil): Add support for per-framework-profile configuration for
// offer acceptance/rejection.
struct FrameworkProfile
{
  FrameworkProfile(
      const string& _name,
      const set<string>& _roles,
      size_t _instances,
      // For frameworks that do not care about launching tasks, we provide
      // some default task launch settings.
      size_t _maxTasksPerInstance = 100,
      const Resources& _taskResources =
        CHECK_NOTERROR(Resources::parse("cpus:1;mem:100")),
      size_t _maxTasksPerOffer = 10)
    : name(_name),
      roles(_roles),
      instances(_instances),
      maxTasksPerInstance(_maxTasksPerInstance),
      taskResources(_taskResources),
      maxTasksPerOffer(_maxTasksPerOffer) {}

  string name;
  set<string> roles;
  size_t instances;

  const size_t maxTasksPerInstance;
  Resources taskResources;
  const size_t maxTasksPerOffer;
};


struct AgentProfile
{
  // TODO(mzhu): Add option to specify `used` resources. `used` resources
  // requires the knowledge of `frameworkId` which currently is created
  // during the initialization which is after the agent profile creation.
  AgentProfile(const string& _name,
               size_t _instances,
               const Resources& _resources)
    : name(_name),
      instances(_instances),
      resources(_resources) {}

  string name;
  size_t instances;
  Resources resources;
};


struct OfferedResources
{
  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources resources;
  string role;
};


struct BenchmarkConfig
{
  BenchmarkConfig(const string& allocator_ = master::DEFAULT_ALLOCATOR,
                  const string& roleSorter_ = "drf",
                  const string& frameworkSorter_ = "drf",
                  const Duration& allocationInterval_ =
                    master::DEFAULT_ALLOCATION_INTERVAL)
    : allocator(allocator_),
      roleSorter(roleSorter_),
      frameworkSorter(frameworkSorter_),
      allocationInterval(allocationInterval_)
  {
    minAllocatableResources.push_back(CHECK_NOTERROR(
        ResourceQuantities::fromString("cpus:" + stringify(MIN_CPUS))));
    minAllocatableResources.push_back(
        CHECK_NOTERROR(ResourceQuantities::fromString(
            "mem:" + stringify((double)MIN_MEM.bytes() / Bytes::MEGABYTES))));
  }

  string allocator;
  string roleSorter;
  string frameworkSorter;

  Duration allocationInterval;

  vector<ResourceQuantities> minAllocatableResources;

  vector<FrameworkProfile> frameworkProfiles;
  vector<AgentProfile> agentProfiles;
};


class HierarchicalAllocations_BenchmarkBase : public ::testing::Test
{
protected:
  HierarchicalAllocations_BenchmarkBase ()
    : totalTasksToLaunch(0) {}

  ~HierarchicalAllocations_BenchmarkBase () override
  {
    delete allocator;
  }

  void initializeCluster(
      const BenchmarkConfig& config,
      Option<lambda::function<
          void(const FrameworkID&,
               const hashmap<string, hashmap<SlaveID, Resources>>&)>>
                 offerCallback = None())
  {
    bool clockPaused = Clock::paused();

    // If clock was not paused, pause the clock so that we could
    // make measurements.
    if (!clockPaused) {
      Clock::pause();
    }

    if (offerCallback.isNone()) {
      offerCallback =
        [this](
            const FrameworkID& frameworkId,
            const hashmap<string, hashmap<SlaveID, Resources>>& resources_) {
          foreachkey (const string& role, resources_) {
            foreachpair (
                const SlaveID& slaveId,
                const Resources& resources,
                resources_.at(role)) {
              offers.put(
                  OfferedResources{frameworkId, slaveId, resources, role});
            }
          }
        };
    }

    allocator = CHECK_NOTERROR(Allocator::create(
        config.allocator, config.roleSorter, config.frameworkSorter));


    Options options;
    options.allocationInterval = config.allocationInterval;
    options.minAllocatableResources = config.minAllocatableResources;

    allocator->initialize(
        options,
        CHECK_NOTNONE(offerCallback),
        {});

    Stopwatch watch;
    watch.start();

    // Add agents.
    size_t agentCount = 0;
    for (const AgentProfile& profile : config.agentProfiles) {
      agentCount += profile.instances;
      for (size_t i = 0; i < profile.instances; i++) {
        const string agentName = profile.name + "-" + stringify(i);

        SlaveInfo agent;
        *(agent.mutable_resources()) = profile.resources;
        agent.mutable_id()->set_value(agentName);
        agent.set_hostname(agentName);

        allocator->addSlave(
            agent.id(),
            agent,
            AGENT_CAPABILITIES(),
            None(),
            agent.resources(),
            {});
      }
    }

    // Wait for all the `addSlave` operations to be processed.
    Clock::settle();

    watch.stop();

    cout << "Added " << agentCount << " agents"
         << " in " << watch.elapsed() << endl;

    // Pause the allocator here to prevent any event-driven allocations while
    // adding frameworks.
    allocator->pause();

    watch.start();

    // Add frameworks.
    size_t frameworkCount = 0;
    for (const FrameworkProfile& profile : config.frameworkProfiles) {
      totalTasksToLaunch += profile.instances * profile.maxTasksPerInstance;
      frameworkCount += profile.instances;

      shared_ptr<FrameworkProfile> sharedProfile =
        make_shared<FrameworkProfile>(profile);

      for (size_t i = 0; i < profile.instances; i++) {
        const string frameworkName = profile.name + "-" + stringify(i);

        FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
        frameworkInfo.set_name(frameworkName);
        frameworkInfo.mutable_id()->set_value(frameworkName);

        frameworkInfo.clear_roles();

        for (const string& role : profile.roles) {
          frameworkInfo.add_roles(role);
        }

        frameworkProfiles[frameworkInfo.id()] = sharedProfile;

        allocator->addFramework(
            frameworkInfo.id(),
            frameworkInfo,
            {},
            true,
            {});
      }
    }

    // Wait for all the `addFramework` operations to be processed.
    Clock::settle();

    watch.stop();

    cout << "Added " << frameworkCount << " frameworks"
         << " in " << watch.elapsed() << endl;

    // Resume the clock if it was not paused.
    if (!clockPaused) {
      Clock::resume();
    }

    allocator->resume();
  }

  const FrameworkProfile& getFrameworkProfile(const FrameworkID& id)
  {
    return *frameworkProfiles[id];
  }

  Allocator* allocator;

  process::Queue<OfferedResources> offers;

  size_t totalTasksToLaunch;

private:
  hashmap<FrameworkID, shared_ptr<FrameworkProfile>> frameworkProfiles;
};


class BENCHMARK_HierarchicalAllocations :
  public HierarchicalAllocations_BenchmarkBase {};


// This benchmark launches frameworks with different profiles (number of tasks,
// task sizes and etc.) and prints out statistics such as total tasks launched,
// cluster utilization and allocation latency. The test has a timeout of 30
// seconds.
TEST_F(BENCHMARK_HierarchicalAllocations, MultiFrameworkAllocations)
{
  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  BenchmarkConfig config;

  // Add agent profiles.
  config.agentProfiles.push_back(AgentProfile(
      "agent",
      80,
      CHECK_NOTERROR(Resources::parse("cpus:64;mem:488000"))));

  // Add framework profiles.

  // A profile to simulate frameworks that launch thousands of smaller apps.
  // It is similar in behavior to some meta-frameworks such as Marathon. This
  // also limits max tasks per offer to 100 to spread the load more uniformly
  // over allocation cycles.
  for (size_t i = 0; i < 4; i++) {
    config.frameworkProfiles.push_back(FrameworkProfile(
        "Marathon-" + stringify(i + 1),
        {"roleA-" + stringify(i + 1)},
        1,
        4000,
        CHECK_NOTERROR(Resources::parse("cpus:0.07;mem:400;")),
        100));
  }

  // A profile to simulate workloads where a large number of frameworks launch a
  // handful of tasks. E.g., a large number of Jenkins masters which are trying
  // to launch some build jobs. We enforce a task-per-offer limit of 1.
  config.frameworkProfiles.push_back(FrameworkProfile(
      "Jenkins",
      {"roleB"},
      500,
      5,
      CHECK_NOTERROR(Resources::parse("cpus:0.1;mem:4000;")),
      1));

  // A profile to simulate workloads where frameworks launch larger jobs that
  // are similar in spirit to Spark dispatcher/driver model. We enforce a
  // task-per-offer limit of 5.
  for (size_t i = 0; i < 50; i++) {
    config.frameworkProfiles.push_back(FrameworkProfile(
        "SparkDispatcher-" + stringify(i + 1),
        {"roleC-" + stringify(i + 1)},
        1,
        20,
        CHECK_NOTERROR(Resources::parse("cpus:1;mem:1000;")),
        5));
  }

  initializeCluster(config);

  // Now perform allocations. We continue until either we timeout or we have
  // launched all of the expected tasks.
  const Duration TEST_TIMEOUT = Seconds(30);

  // Total tasks launched per framework. Used to enforce task caps.
  hashmap<FrameworkID, size_t> frameworkTasksLaunched;

  Stopwatch totalTime;
  totalTime.start();

  size_t allocationCount = 0;
  size_t totalTasksLaunched = 0;
  Resources clusterAllocation;

  while (totalTasksLaunched < totalTasksToLaunch &&
         totalTime.elapsed() < TEST_TIMEOUT) {
    Stopwatch watch;
    watch.start();

    // Advance the clock and trigger a batch allocation cycle.
    Clock::advance(config.allocationInterval);
    Clock::settle();

    watch.stop();

    allocationCount++;

    Future<OfferedResources> offer_ = offers.get();
    size_t offerCount = 0;

    while (offer_.isReady()) {
      const OfferedResources& offer = offer_.get();
      const FrameworkID& frameworkId = offer.frameworkId;
      const FrameworkProfile& frameworkProfile =
        getFrameworkProfile(frameworkId);

      offerCount++;

      Resources remainingResources = offer.resources;

      // We strip allocation information of `remainingResources` so that we
      // can compare/subtract with `frameworkProfile.taskResources`.
      remainingResources.unallocate();

      size_t tasksLaunched = 0;

      while (remainingResources.contains(frameworkProfile.taskResources) &&
             frameworkTasksLaunched[frameworkId] <
               frameworkProfile.maxTasksPerInstance &&
             tasksLaunched < frameworkProfile.maxTasksPerOffer) {
        remainingResources -= frameworkProfile.taskResources;
        frameworkTasksLaunched[frameworkId]++;
        totalTasksLaunched++;
        tasksLaunched++;
        clusterAllocation += frameworkProfile.taskResources;
      }

      // We restore the allocation information to recover the resources.
      remainingResources.allocate(offer.role);

      allocator->recoverResources(
          frameworkId,
          offer.slaveId,
          remainingResources,
          None(),
          false);

      offer_ = offers.get();
    }

    cout << "Launched " << totalTasksLaunched << " tasks out of "
         << totalTasksToLaunch << " total tasks in "
         << allocationCount << " rounds. Current allocation round generated "
         << offerCount << " offers and took " << watch.elapsed() << endl;
  }

  if (totalTasksLaunched < totalTasksToLaunch) {
    cout << "Failed to launch all tasks: Timed out after "
         << TEST_TIMEOUT << endl;
  }

  // Compute and print statistics around cluster capacity, cluster allocation,
  // and allocation target.

  Resources clusterCapacity;
  for (const AgentProfile& profile : config.agentProfiles) {
    for (size_t i = 0; i < profile.instances; i++) {
        clusterCapacity += profile.resources;
    }
  }

  Resources targetAllocation;
  for (const FrameworkProfile& profile : config.frameworkProfiles) {
    for (size_t i = 0; i < profile.instances; i++) {
      for (size_t j = 0; j < profile.maxTasksPerInstance; j++) {
        targetAllocation += profile.taskResources;
      }
    }
  }

  cout << "Resource statistics:\n";
  cout << "Cluster capacity: " << clusterCapacity << endl;
  cout << "Cluster allocation: " << clusterAllocation << endl;
  cout << "Target allocation: " << targetAllocation << endl;
}


struct QuotaParam
{
  QuotaParam(
      const size_t _smallQuotaRoleCount,
      const size_t _largeQuotaRoleCount,
      const size_t _frameworksPerRole)
    : smallQuotaRoleCount(_smallQuotaRoleCount),
      largeQuotaRoleCount(_largeQuotaRoleCount),
      frameworksPerRole(_frameworksPerRole) {}

  QuotaParam() = default;

  // `smallQuotaRole` will have a resource quota of 1/5 an agent, this will
  // lead to agent chopping.
  //
  // `largeQuotaRole` will have a quota of 5 agents, this will lead to
  // role unsatisfied quota chopping.
  //
  // Together, this determines the number of agents to be:
  //
  //  `smallQuotaRoleCount / 5 + largeQuotaRoleCount * 5`
  //
  // TODO(mzhu): Consider adding another parameter to control the chopping.

  size_t smallQuotaRoleCount;
  size_t largeQuotaRoleCount;

  // Number of frameworks per role.
  size_t frameworksPerRole;
};


class BENCHMARK_HierarchicalAllocator_WithQuotaParam
  : public HierarchicalAllocations_BenchmarkBase,
    public WithParamInterface<std::tuple<const char*, QuotaParam>> {};


// Since the quota performance is mostly decided by number of agents and
// roles, in the default setting below, we let each role has a single framework.
INSTANTIATE_TEST_CASE_P(
    QuotaParam,
    BENCHMARK_HierarchicalAllocator_WithQuotaParam,
    ::testing::Combine(
        ::testing::Values("drf", "random"),
        ::testing::Values(
            QuotaParam(25U, 5U, 1U), // 30 agents in total
            QuotaParam(250U, 50U, 1U), // 300 agents in total
            QuotaParam(2500U, 500U, 1U)))); // 3000 agents in total


// This benchmark evaluates the allocator performance in the presence of
// roles with both small quota (which can be satisfied by half an agent) as
// well as large quota (which need resources from two agents). We setup the
// cluster, trigger one allocation cycle and measure the elapsed time. All
// cluster resources will be offered in this cycle to satisfy all roles' quota.
TEST_P(BENCHMARK_HierarchicalAllocator_WithQuotaParam, LargeAndSmallQuota)
{
  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  // Each agent can satisfy either two `smallQuotaRole` or half of
  // `largeQuotaRole`.
  const string agentResourcesString = "cpus:2;mem:2048;disk:2048";
  const string smallQuotaResourcesString = "cpus:1;mem:1024;disk:1024";
  const string largeQuotaResourcesString = "cpus:4;mem:4096;disk:4096";

  const string& sorter = std::get<0>(GetParam());

  BenchmarkConfig config{master::DEFAULT_ALLOCATOR, sorter, sorter};

  const QuotaParam& quotaParam = std::get<1>(GetParam());

  // Store roles for setting up quota later (after allocator is initialized).
  vector<string> smallQuotaRoles;

  // Add framework profiles for `smallQuotaRole`.
  for (size_t i = 0; i < quotaParam.smallQuotaRoleCount; i++) {
    string role("smallQuotaRole" + stringify(i));
    smallQuotaRoles.push_back(role);
    config.frameworkProfiles.push_back(FrameworkProfile(
        "framework_small_" + stringify(i),
        {role},
        quotaParam.frameworksPerRole));
  }

  // Store roles for setting up quota later (after allocator is initialized).
  vector<string> largeQuotaRoles;

  // Add framework profiles for `largeQuotaRole`.
  for (size_t i = 0; i < quotaParam.largeQuotaRoleCount; i++) {
    string role("largeQuotaRole" + stringify(i));
    largeQuotaRoles.push_back(role);
    config.frameworkProfiles.push_back(FrameworkProfile(
        "framework_large_" + stringify(i),
        {role},
        quotaParam.frameworksPerRole));
  }

  // See comment above in `QuotaParam` regarding how we decide agent counts.
  size_t agentCount =
    quotaParam.smallQuotaRoleCount / 5 + quotaParam.largeQuotaRoleCount * 5;

  // Add agent profiles.
  config.agentProfiles.push_back(AgentProfile(
      "agent",
      agentCount,
      CHECK_NOTERROR(Resources::parse(agentResourcesString))));

  initializeCluster(config);

  size_t totalRoleCount =
    quotaParam.smallQuotaRoleCount + quotaParam.largeQuotaRoleCount;

  cout << "Benchmark setup: " << agentCount << " agents, " << totalRoleCount
       << " roles, " << totalRoleCount * quotaParam.frameworksPerRole
       << " frameworks, with " << sorter << " sorter" << endl;

  // Pause the allocator here to prevent any event-driven allocations while
  // setting up the quota (while setting quota currently does not lead to
  // event-driven allocations, this behavior might change in the future).
  allocator->pause();

  foreach (const string& role, smallQuotaRoles) {
    allocator->updateQuota(role, createQuota(smallQuotaResourcesString));
  }

  foreach (const string& role, largeQuotaRoles) {
    allocator->updateQuota(role, createQuota(largeQuotaResourcesString));
  }

  allocator->resume();

  Stopwatch watch;
  watch.start();

  // Advance the clock and trigger a batch allocation cycle.
  Clock::advance(config.allocationInterval);
  Clock::settle();

  watch.stop();

  size_t offerCount = 0;

  while (offers.get().isReady()) {
    ++offerCount;
  }

  cout << "Made " << offerCount << " allocations in " << watch.elapsed()
       << endl;

  watch.start();

  // Advance the clock and trigger a batch allocation cycle.
  Clock::advance(config.allocationInterval);
  Clock::settle();

  watch.stop();

  // No allocations should be made because all resources were allocated in the
  // first round.
  EXPECT_TRUE(offers.get().isPending());

  cout << "Made 0 allocation in " << watch.elapsed() << endl;
}


struct NonQuotaVsQuotaParam
{
  NonQuotaVsQuotaParam(
      const size_t _roleCount,
      const size_t _agentsPerRole,
      const size_t _frameworksPerRole,
      const bool _setQuota)
    : roleCount(_roleCount),
      agentsPerRole(_agentsPerRole),
      frameworksPerRole(_frameworksPerRole),
      setQuota(_setQuota) {}

  NonQuotaVsQuotaParam() = default;

  size_t roleCount;

  // This determines the number of agents needed to satisfy a role's quota.
  // And total number of agents in the cluster will be
  // roleCount * agentsPerRole.
  size_t agentsPerRole;

  size_t frameworksPerRole;

  bool setQuota;
};


class BENCHMARK_HierarchicalAllocator_WithNonQuotaVsQuotaParam
  : public HierarchicalAllocations_BenchmarkBase,
    public WithParamInterface<std::tuple<const char*, NonQuotaVsQuotaParam>> {};


INSTANTIATE_TEST_CASE_P(
    NonQuotaVsQuotaParam,
    BENCHMARK_HierarchicalAllocator_WithNonQuotaVsQuotaParam,
    ::testing::Combine(
      ::testing::Values("drf", "random"),
      ::testing::Values(
        // 10 roles, 10*2 = 20 agents, 10*2 = 20 frameworks,
        // without and with quota.
        NonQuotaVsQuotaParam(10U, 2U, 2U, false),
        NonQuotaVsQuotaParam(10U, 2U, 2U, true),
        // 100 roles, 100*2 = 200 agents, 100*2 = 200 frameworks.
        // without and with quota.
        NonQuotaVsQuotaParam(100U, 2U, 2U, false),
        NonQuotaVsQuotaParam(100U, 2U, 2U, true),
        // 1000 roles, 1000*2 = 2000 agents, 1000*2 = 2000 frameworks.
        // without and with quota.
        NonQuotaVsQuotaParam(1000U, 2U, 2U, false),
        NonQuotaVsQuotaParam(1000U, 2U, 2U, true))));


// This benchmark evaluates the performance difference between nonquota
// and quota settings. In both settings, the same allocations are made
// for fair comparison. In particular, since the agent will always be
// allocated as a whole in nonquota settings, we should also avoid
// agent chopping in quota setting as well. Thus in this benchmark,
// quotas are only set to be multiples of whole agent resources.
// This is also why we have this dedicated benchmark for comparison
// rather than extending the existing quota benchmarks (which involves
// agent chopping).
TEST_P(
    BENCHMARK_HierarchicalAllocator_WithNonQuotaVsQuotaParam, NonQuotaVsQuota)
{
  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  const string agentResourcesString = "cpus:2;mem:2048;disk:2048";

  const string& sorter = std::get<0>(GetParam());

  BenchmarkConfig config{master::DEFAULT_ALLOCATOR, sorter, sorter};

  const NonQuotaVsQuotaParam& quotaParam = std::get<1>(GetParam());

  // Store roles for setting up quota later if needed.
  vector<string> roles;

  for (size_t i = 0; i < quotaParam.roleCount; i++) {
    string role("role" + stringify(i));
    roles.push_back(role);
    config.frameworkProfiles.push_back(FrameworkProfile(
        "framework_" + stringify(i), {role}, quotaParam.frameworksPerRole));
  }

  size_t agentCount = quotaParam.roleCount * quotaParam.agentsPerRole;

  // Add agent profiles.
  config.agentProfiles.push_back(AgentProfile(
      "agent",
      agentCount,
      CHECK_NOTERROR(Resources::parse(agentResourcesString))));

  initializeCluster(config);

  if (quotaParam.setQuota) {
    // We ensure the same allocations are made for both nonquota and quota
    // settings for fair comparison. Thus quota is set to consume multiple
    // agents and each agent will be offered as a whole.
    const string quotaResourcesString =
      "cpus:" + stringify(2 * quotaParam.agentsPerRole) +
      ";mem:" + stringify(2048 * quotaParam.agentsPerRole) +
      ";disk:" + stringify(2048 * quotaParam.agentsPerRole);

    // Pause the allocator here to prevent any event-driven allocations while
    // setting up the quota (while setting quota currently does not lead to
    // event-driven allocations, this behavior might change in the future).
    allocator->pause();

    foreach (const string& role, roles) {
      allocator->updateQuota(role, createQuota(quotaResourcesString));
    }

    allocator->resume();

    cout << "Quota run setup: ";
  } else {
    cout << "Nonquota run setup: ";
  }

  cout << agentCount << " agents, " << quotaParam.roleCount << " roles, "
       << quotaParam.roleCount * quotaParam.frameworksPerRole << " frameworks,"
       << " with " << sorter << " sorter " << endl;

  Stopwatch watch;
  watch.start();

  // Advance the clock and trigger a batch allocation cycle.
  Clock::advance(config.allocationInterval);
  Clock::settle();

  watch.stop();

  size_t offerCount = 0;

  while (offers.get().isReady()) {
    offerCount++;
  }

  cout << "Made " << offerCount << " allocations in " << watch.elapsed()
       << endl;

  watch.start();

  // Advance the clock and trigger a batch allocation cycle.
  Clock::advance(config.allocationInterval);
  Clock::settle();

  watch.stop();

  // No allocations should be made because all resources were allocated in the
  // first round.
  EXPECT_TRUE(offers.get().isPending());

  cout << "Made 0 allocation in " << watch.elapsed() << endl;
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
