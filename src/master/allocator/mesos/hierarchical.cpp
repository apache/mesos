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

#include "master/allocator/mesos/hierarchical.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/id.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/hashset.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>

#include "common/protobuf_utils.hpp"

using std::set;
using std::string;
using std::vector;

using mesos::allocator::InverseOfferStatus;

using process::Failure;
using process::Future;
using process::Owned;
using process::Timeout;

using mesos::internal::protobuf::framework::Capabilities;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {
namespace internal {

// Used to represent "filters" for resources unused in offers.
class OfferFilter
{
public:
  virtual ~OfferFilter() {}

  virtual bool filter(const Resources& resources) const = 0;
};


class RefusedOfferFilter : public OfferFilter
{
public:
  RefusedOfferFilter(const Resources& _resources) : resources(_resources) {}

  virtual bool filter(const Resources& _resources) const
  {
    // TODO(jieyu): Consider separating the superset check for regular
    // and revocable resources. For example, frameworks might want
    // more revocable resources only or non-revocable resources only,
    // but currently the filter only expires if there is more of both
    // revocable and non-revocable resources.
    return resources.contains(_resources); // Refused resources are superset.
  }

private:
  const Resources resources;
};


// Used to represent "filters" for inverse offers.
//
// NOTE: Since this specific allocator implementation only sends inverse offers
// for maintenance primitives, and those are at the whole slave level, we only
// need to filter based on the time-out.
// If this allocator implementation starts sending out more resource specific
// inverse offers, then we can capture the `unavailableResources` in the filter
// function.
class InverseOfferFilter
{
public:
  virtual ~InverseOfferFilter() {}

  virtual bool filter() const = 0;
};


// NOTE: See comment above `InverseOfferFilter` regarding capturing
// `unavailableResources` if this allocator starts sending fine-grained inverse
// offers.
class RefusedInverseOfferFilter : public InverseOfferFilter
{
public:
  RefusedInverseOfferFilter(const Timeout& _timeout)
    : timeout(_timeout) {}

  virtual bool filter() const
  {
    // See comment above why we currently don't do more fine-grained filtering.
    return timeout.remaining() > Seconds(0);
  }

private:
  const Timeout timeout;
};


HierarchicalAllocatorProcess::Framework::Framework(
    const FrameworkInfo& frameworkInfo)
  : roles(protobuf::framework::getRoles(frameworkInfo)),
    capabilities(frameworkInfo.capabilities()) {}


void HierarchicalAllocatorProcess::initialize(
    const Duration& _allocationInterval,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<string, hashmap<SlaveID, Resources>>&)>&
      _offerCallback,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, UnavailableResources>&)>&
      _inverseOfferCallback,
    const Option<set<string>>& _fairnessExcludeResourceNames)
{
  allocationInterval = _allocationInterval;
  offerCallback = _offerCallback;
  inverseOfferCallback = _inverseOfferCallback;
  fairnessExcludeResourceNames = _fairnessExcludeResourceNames;
  initialized = true;
  paused = false;

  // Resources for quota'ed roles are allocated separately and prior to
  // non-quota'ed roles, hence a dedicated sorter for quota'ed roles is
  // necessary.
  roleSorter->initialize(fairnessExcludeResourceNames);
  quotaRoleSorter->initialize(fairnessExcludeResourceNames);

  VLOG(1) << "Initialized hierarchical allocator process";

  delay(allocationInterval, self(), &Self::batch);
}


void HierarchicalAllocatorProcess::recover(
    const int _expectedAgentCount,
    const hashmap<string, Quota>& quotas)
{
  // Recovery should start before actual allocation starts.
  CHECK(initialized);
  CHECK(slaves.empty());
  CHECK_EQ(0, quotaRoleSorter->count());
  CHECK(_expectedAgentCount >= 0);

  // If there is no quota, recovery is a no-op. Otherwise, we need
  // to delay allocations while agents are re-registering because
  // otherwise we perform allocations on a partial view of resources!
  // We would consequently perform unnecessary allocations to satisfy
  // quota constraints, which can over-allocate non-revocable resources
  // to roles using quota. Then, frameworks in roles without quota can
  // be unnecessarily deprived of resources. We may also be unable to
  // satisfy all of the quota constraints. Repeated master failovers
  // exacerbate the issue.

  if (quotas.empty()) {
    VLOG(1) << "Skipping recovery of hierarchical allocator: "
            << "nothing to recover";

    return;
  }

  // NOTE: `quotaRoleSorter` is updated implicitly in `setQuota()`.
  foreachpair (const string& role, const Quota& quota, quotas) {
    setQuota(role, quota);
  }

  // TODO(alexr): Consider exposing these constants.
  const Duration ALLOCATION_HOLD_OFF_RECOVERY_TIMEOUT = Minutes(10);
  const double AGENT_RECOVERY_FACTOR = 0.8;

  // Record the number of expected agents.
  expectedAgentCount =
    static_cast<int>(_expectedAgentCount * AGENT_RECOVERY_FACTOR);

  // Skip recovery if there are no expected agents. This is not strictly
  // necessary for the allocator to function correctly, but maps better
  // to expected behavior by the user: the allocator is not paused until
  // a new agent is added.
  if (expectedAgentCount.get() == 0) {
    VLOG(1) << "Skipping recovery of hierarchical allocator: "
            << "no reconnecting agents to wait for";

    return;
  }

  // Pause allocation until after a sufficient amount of agents reregister
  // or a timer expires.
  pause();

  // Setup recovery timer.
  delay(ALLOCATION_HOLD_OFF_RECOVERY_TIMEOUT, self(), &Self::resume);

  LOG(INFO) << "Triggered allocator recovery: waiting for "
            << expectedAgentCount.get() << " agents to reconnect or "
            << ALLOCATION_HOLD_OFF_RECOVERY_TIMEOUT << " to pass";
}


void HierarchicalAllocatorProcess::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const hashmap<SlaveID, Resources>& used,
    bool active)
{
  CHECK(initialized);
  CHECK(!frameworks.contains(frameworkId));

  frameworks.insert({frameworkId, Framework(frameworkInfo)});

  const Framework& framework = frameworks.at(frameworkId);

  foreach (const string& role, framework.roles) {
    trackFrameworkUnderRole(frameworkId, role);

    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->activate(frameworkId.value());
  }

  // TODO(bmahler): Validate that the reserved resources have the
  // framework's role.

  // Update the allocation for this framework.
  foreachpair (const SlaveID& slaveId, const Resources& resources, used) {
    if (!slaves.contains(slaveId)) {
      continue;
    }

    hashmap<string, Resources> allocations = resources.allocations();

    foreachpair (const string& role, const Resources& allocation, allocations) {
      roleSorter->allocated(role, slaveId, allocation);
      frameworkSorters.at(role)->add(slaveId, allocation);
      frameworkSorters.at(role)->allocated(
          frameworkId.value(), slaveId, allocation);

      if (quotas.contains(role)) {
        // See comment at `quotaRoleSorter` declaration
        // regarding non-revocable.
        quotaRoleSorter->allocated(role, slaveId, allocation.nonRevocable());
      }
    }
  }

  LOG(INFO) << "Added framework " << frameworkId;

  if (active) {
    allocate();
  } else {
    deactivateFramework(frameworkId);
  }
}


void HierarchicalAllocatorProcess::removeFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  const Framework& framework = frameworks.at(frameworkId);

  foreach (const string& role, framework.roles) {
    // Might not be in 'frameworkSorters[role]' because it
    // was previously deactivated and never re-added.
    if (!frameworkSorters.contains(role) ||
        !frameworkSorters.at(role)->contains(frameworkId.value())) {
      continue;
    }

    hashmap<SlaveID, Resources> allocation =
      frameworkSorters.at(role)->allocation(frameworkId.value());

    // Update the allocation for this framework.
    foreachpair (const SlaveID& slaveId,
                 const Resources& allocated,
                 allocation) {
      roleSorter->unallocated(role, slaveId, allocated);
      frameworkSorters.at(role)->remove(slaveId, allocated);

      if (quotas.contains(role)) {
        // See comment at `quotaRoleSorter` declaration
        // regarding non-revocable.
        quotaRoleSorter->unallocated(role, slaveId, allocated.nonRevocable());
      }
    }

    untrackFrameworkUnderRole(frameworkId, role);
  }

  // Do not delete the filters contained in this
  // framework's `offerFilters` hashset yet, see comments in
  // HierarchicalAllocatorProcess::reviveOffers and
  // HierarchicalAllocatorProcess::expire.
  frameworks.erase(frameworkId);

  LOG(INFO) << "Removed framework " << frameworkId;
}


void HierarchicalAllocatorProcess::activateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  const Framework& framework = frameworks.at(frameworkId);

  foreach (const string& role, framework.roles) {
    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->activate(frameworkId.value());
  }

  LOG(INFO) << "Activated framework " << frameworkId;

  allocate();
}


void HierarchicalAllocatorProcess::deactivateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  Framework& framework = frameworks.at(frameworkId);

  foreach (const string& role, framework.roles) {
    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->deactivate(frameworkId.value());

    // Note that the Sorter *does not* remove the resources allocated
    // to this framework. For now, this is important because if the
    // framework fails over and is activated, we still want a record
    // of the resources that it is using. We might be able to collapse
    // the added/removed and activated/deactivated in the future.
  }

  // Do not delete the filters contained in this
  // framework's `offerFilters` hashset yet, see comments in
  // HierarchicalAllocatorProcess::reviveOffers and
  // HierarchicalAllocatorProcess::expire.
  framework.offerFilters.clear();
  framework.inverseOfferFilters.clear();

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


void HierarchicalAllocatorProcess::updateFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  Framework& framework = frameworks.at(frameworkId);

  set<string> oldRoles = framework.roles;
  set<string> newRoles = protobuf::framework::getRoles(frameworkInfo);

  const set<string> removedRoles = [&]() {
    set<string> result = oldRoles;
    foreach (const string& role, newRoles) {
      result.erase(role);
    }
    return result;
  }();

  foreach (const string& role, removedRoles) {
    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->deactivate(frameworkId.value());

    // Stop tracking the framework under this role if there are
    // no longer any resources allocated to it.
    if (frameworkSorters.at(role)->allocation(frameworkId.value()).empty()) {
      untrackFrameworkUnderRole(frameworkId, role);
    }

    if (framework.offerFilters.contains(role)) {
      framework.offerFilters.erase(role);
    }
  }

  const set<string> addedRoles = [&]() {
    set<string> result = newRoles;
    foreach (const string& role, oldRoles) {
      result.erase(role);
    }
    return result;
  }();

  foreach (const string& role, addedRoles) {
    // NOTE: It's possible that we're already tracking this framework
    // under the role because a framework can unsubscribe from a role
    // while it still has resources allocated to the role.
    if (!isFrameworkTrackedUnderRole(frameworkId, role)) {
      trackFrameworkUnderRole(frameworkId, role);
    }

    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->activate(frameworkId.value());
  }

  framework.roles = newRoles;
  framework.capabilities = frameworkInfo.capabilities();
}


void HierarchicalAllocatorProcess::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const vector<SlaveInfo::Capability>& capabilities,
    const Option<Unavailability>& unavailability,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);
  CHECK(!slaves.contains(slaveId));
  CHECK(!paused || expectedAgentCount.isSome());

  roleSorter->add(slaveId, total);

  // See comment at `quotaRoleSorter` declaration regarding non-revocable.
  quotaRoleSorter->add(slaveId, total.nonRevocable());

  // Update the allocation for each framework.
  foreachpair (const FrameworkID& frameworkId,
               const Resources& used_,
               used) {
    if (!frameworks.contains(frameworkId)) {
      continue;
    }

    foreachpair (const string& role,
                 const Resources& allocated,
                 used_.allocations()) {
      // The framework has resources allocated to this role but it may
      // or may not be subscribed to the role. Either way, we need to
      // track the framework under the role.
      if (!isFrameworkTrackedUnderRole(frameworkId, role)) {
        trackFrameworkUnderRole(frameworkId, role);
      }

      // TODO(bmahler): Validate that the reserved resources have the
      // framework's role.
      CHECK(roleSorter->contains(role));
      CHECK(frameworkSorters.contains(role));
      CHECK(frameworkSorters.at(role)->contains(frameworkId.value()));

      roleSorter->allocated(role, slaveId, allocated);
      frameworkSorters.at(role)->add(slaveId, allocated);
      frameworkSorters.at(role)->allocated(
          frameworkId.value(), slaveId, allocated);

      if (quotas.contains(role)) {
        // See comment at `quotaRoleSorter` declaration regarding non-revocable.
        quotaRoleSorter->allocated(role, slaveId, allocated.nonRevocable());
      }
    }
  }

  slaves[slaveId] = Slave();

  Slave& slave = slaves.at(slaveId);

  slave.total = total;
  slave.allocated = Resources::sum(used);
  slave.activated = true;
  slave.hostname = slaveInfo.hostname();
  slave.capabilities = protobuf::slave::Capabilities(capabilities);

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.
  if (unavailability.isSome()) {
    slave.maintenance = Slave::Maintenance(unavailability.get());
  }

  // If we have just a number of recovered agents, we cannot distinguish
  // between "old" agents from the registry and "new" ones joined after
  // recovery has started. Because we do not persist enough information
  // to base logical decisions on, any accounting algorithm here will be
  // crude. Hence we opted for checking whether a certain amount of cluster
  // capacity is back online, so that we are reasonably confident that we
  // will not over-commit too many resources to quota that we will not be
  // able to revoke.
  if (paused &&
      expectedAgentCount.isSome() &&
      (static_cast<int>(slaves.size()) >= expectedAgentCount.get())) {
    VLOG(1) << "Recovery complete: sufficient amount of agents added; "
            << slaves.size() << " agents known to the allocator";

    expectedAgentCount = None();
    resume();
  }

  LOG(INFO) << "Added agent " << slaveId << " (" << slave.hostname << ")"
            << " with " << slave.total
            << " (allocated: " << slave.allocated << ")";

  allocate(slaveId);
}


void HierarchicalAllocatorProcess::removeSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  // TODO(bmahler): Per MESOS-621, this should remove the allocations
  // that any frameworks have on this slave. Otherwise the caller may
  // "leak" allocated resources accidentally if they forget to recover
  // all the resources. Fixing this would require more information
  // than what we currently track in the allocator.

  roleSorter->remove(slaveId, slaves.at(slaveId).total);

  // See comment at `quotaRoleSorter` declaration regarding non-revocable.
  quotaRoleSorter->remove(slaveId, slaves.at(slaveId).total.nonRevocable());

  slaves.erase(slaveId);
  allocationCandidates.erase(slaveId);

  // Note that we DO NOT actually delete any filters associated with
  // this slave, that will occur when the delayed
  // HierarchicalAllocatorProcess::expire gets invoked (or the framework
  // that applied the filters gets removed).

  LOG(INFO) << "Removed agent " << slaveId;
}


void HierarchicalAllocatorProcess::updateSlave(
    const SlaveID& slaveId,
    const Option<Resources>& oversubscribed,
    const Option<vector<SlaveInfo::Capability>>& capabilities)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  Slave& slave = slaves.at(slaveId);

  bool updated = false;

  // Update agent capabilities.
  if (capabilities.isSome()) {
    protobuf::slave::Capabilities newCapabilities(capabilities.get());
    protobuf::slave::Capabilities oldCapabilities(slave.capabilities);

    slave.capabilities = newCapabilities;

    if (newCapabilities != oldCapabilities) {
      updated = true;

      LOG(INFO) << "Agent " << slaveId << " (" << slave.hostname << ")"
                << " updated with capabilities " << slave.capabilities;
    }
  }

  if (oversubscribed.isSome()) {
    // Check that all the oversubscribed resources are revocable.
    CHECK_EQ(oversubscribed.get(), oversubscribed->revocable());

    const Resources oldRevocable = slave.total.revocable();

    if (oldRevocable != oversubscribed.get()) {
      // Update the total resources.
      //
      // Reset the total resources to include the non-revocable resources,
      // plus the new estimate of oversubscribed resources.
      //
      // NOTE: All modifications to revocable resources in the allocator for
      // `slaveId` are lost.
      //
      // TODO(alexr): Update this math once the source of revocable resources
      // is extended beyond oversubscription.
      slave.total = slave.total.nonRevocable() + oversubscribed.get();

      // Update the total resources in the `roleSorter` by removing the
      // previous oversubscribed resources and adding the new
      // oversubscription estimate.
      roleSorter->remove(slaveId, oldRevocable);
      roleSorter->add(slaveId, oversubscribed.get());

      updated = true;

      // NOTE: We do not need to update `quotaRoleSorter` because this
      // function only changes the revocable resources on the slave, but
      // the quota role sorter only manages non-revocable resources.

      LOG(INFO) << "Agent " << slaveId << " (" << slave.hostname << ")"
                << " updated with oversubscribed resources "
                << oversubscribed.get() << " (total: " << slave.total
                << ", allocated: " << slave.allocated << ")";
    }
  }

  if (updated) {
    allocate(slaveId);
  }
}


void HierarchicalAllocatorProcess::activateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves.at(slaveId).activated = true;

  LOG(INFO) << "Agent " << slaveId << " reactivated";
}


void HierarchicalAllocatorProcess::deactivateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves.at(slaveId).activated = false;

  LOG(INFO) << "Agent " << slaveId << " deactivated";
}


void HierarchicalAllocatorProcess::updateWhitelist(
    const Option<hashset<string>>& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated agent whitelist: " << stringify(whitelist.get());

    if (whitelist.get().empty()) {
      LOG(WARNING) << "Whitelist is empty, no offers will be made!";
    }
  } else {
    LOG(INFO) << "Advertising offers for all agents";
  }
}


void HierarchicalAllocatorProcess::requestResources(
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


void HierarchicalAllocatorProcess::updateAllocation(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& offeredResources,
    const vector<Offer::Operation>& operations)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));
  CHECK(frameworks.contains(frameworkId));

  Slave& slave = slaves.at(slaveId);

  // We require that an allocation is tied to a single role.
  //
  // TODO(bmahler): The use of `Resources::allocations()` induces
  // unnecessary copying of `Resources` objects (which is expensive
  // at the time this was written).
  hashmap<string, Resources> allocations = offeredResources.allocations();

  CHECK_EQ(1u, allocations.size());

  string role = allocations.begin()->first;

  CHECK(frameworkSorters.contains(role));

  const Owned<Sorter>& frameworkSorter = frameworkSorters.at(role);
  const Resources frameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  // We keep a copy of the offered resources here and it is updated
  // by the operations.
  Resources updatedOfferedResources = offeredResources;

  // Accumulate consumed resources for all tasks in all `LAUNCH` operations.
  //
  // For LAUNCH operations we support tasks requesting more instances of
  // shared resources than those being offered. We keep track of total
  // consumed resources to determine the additional instances and allocate
  // them as part of updating the framework's allocation (i.e., add
  // them to the allocated resources in the allocator and in each
  // of the sorters).
  Resources consumed;

  // Used for logging.
  hashset<TaskID> taskIds;

  foreach (const Offer::Operation& operation, operations) {
    // The operations should have been normalized by the master via
    // `protobuf::injectAllocationInfo()`.
    //
    // TODO(bmahler): Check that the operations have the allocation
    // info set. The master should enforce this. E.g.
    //
    //  foreach (const Offer::Operation& operation, operations) {
    //    CHECK_NONE(validateOperationOnAllocatedResources(operation));
    //  }

    // Update the offered resources based on this operation.
    Try<Resources> _updatedOfferedResources = updatedOfferedResources.apply(
        operation);

    CHECK_SOME(_updatedOfferedResources);
    updatedOfferedResources = _updatedOfferedResources.get();

    if (operation.type() == Offer::Operation::LAUNCH) {
      foreach (const TaskInfo& task, operation.launch().task_infos()) {
        taskIds.insert(task.task_id());

        // For now we only need to look at the task resources and
        // ignore the executor resources.
        //
        // TODO(anindya_sinha): For simplicity we currently don't
        // allow shared resources in ExecutorInfo. The reason is that
        // the allocator has no idea if the executor within the task
        // represents a new executor. Therefore we cannot reliably
        // determine if the executor resources are needed for this task.
        // The TODO is to support it. We need to pass in the information
        // pertaining to the executor before enabling shared resources
        // in the executor.
        consumed += task.resources();
      }
    }
  }

  // Check that offered resources contain at least one copy of each
  // consumed shared resource (guaranteed by master validation).
  Resources consumedShared = consumed.shared();
  Resources updatedOfferedShared = updatedOfferedResources.shared();

  foreach (const Resource& resource, consumedShared) {
    CHECK(updatedOfferedShared.contains(resource));
  }

  // Determine the additional instances of shared resources needed to be
  // added to the allocations.
  Resources additional = consumedShared - updatedOfferedShared;

  if (!additional.empty()) {
    LOG(INFO) << "Allocating additional resources " << additional
              << " for tasks " << stringify(taskIds)
              << " of framework " << frameworkId << " on agent " << slaveId;

    updatedOfferedResources += additional;
  }

  // Update the per-slave allocation.
  slave.allocated -= offeredResources;
  slave.allocated += updatedOfferedResources;

  // Update the allocation in the framework sorter.
  frameworkSorter->update(
      frameworkId.value(),
      slaveId,
      offeredResources,
      updatedOfferedResources);

  // Update the allocation in the role sorter.
  roleSorter->update(
      role,
      slaveId,
      offeredResources,
      updatedOfferedResources);

  // Update the allocated resources in the quota sorter. We only update
  // the allocated resources if this role has quota set.
  if (quotas.contains(role)) {
    // See comment at `quotaRoleSorter` declaration regarding non-revocable.
    quotaRoleSorter->update(
        role,
        slaveId,
        offeredResources.nonRevocable(),
        updatedOfferedResources.nonRevocable());
  }

  // Update the agent total resources so they are consistent with the updated
  // allocation. We do not directly use `updatedOfferedResources` here because
  // the agent's total resources shouldn't contain:
  // 1. The additionally allocated shared resources.
  // 2. `AllocationInfo` as set in `updatedOfferedResources`.

  // We strip `AllocationInfo` from operations in order to apply them
  // successfully, since agent's total is stored as unallocated resources.
  vector<Offer::Operation> strippedOperations = operations;
  foreach (Offer::Operation& operation, strippedOperations) {
    protobuf::stripAllocationInfo(&operation);
  }

  Try<Resources> updatedTotal = slave.total.apply(strippedOperations);
  CHECK_SOME(updatedTotal);
  updateSlaveTotal(slaveId, updatedTotal.get());

  // Update the total resources in the framework sorter.
  frameworkSorter->remove(slaveId, offeredResources);
  frameworkSorter->add(slaveId, updatedOfferedResources);

  // Check that the `flattened` quantities for framework allocations
  // have not changed by the above operations.
  const Resources updatedFrameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  CHECK_EQ(
      frameworkAllocation.flatten().createStrippedScalarQuantity(),
      updatedFrameworkAllocation.flatten().createStrippedScalarQuantity());

  LOG(INFO) << "Updated allocation of framework " << frameworkId
            << " on agent " << slaveId
            << " from " << frameworkAllocation
            << " to " << updatedFrameworkAllocation;
}


Future<Nothing> HierarchicalAllocatorProcess::updateAvailable(
    const SlaveID& slaveId,
    const vector<Offer::Operation>& operations)
{
  // Note that the operations may contain allocated resources,
  // however such operations can be applied to unallocated
  // resources unambiguously, so we don't have a strict CHECK
  // for the operations to contain only unallocated resources.

  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  Slave& slave = slaves.at(slaveId);

  // It's possible for this 'apply' to fail here because a call to
  // 'allocate' could have been enqueued by the allocator itself
  // just before master's request to enqueue 'updateAvailable'
  // arrives to the allocator.
  //
  //   Master -------R------------
  //                  \----+
  //                       |
  //   Allocator --A-----A-U---A--
  //                \___/ \___/
  //
  //   where A = allocate, R = reserve, U = updateAvailable
  Try<Resources> updatedAvailable = slave.available().apply(operations);
  if (updatedAvailable.isError()) {
    return Failure(updatedAvailable.error());
  }

  // Update the total resources.
  Try<Resources> updatedTotal = slave.total.apply(operations);
  CHECK_SOME(updatedTotal);

  // Update the total resources in the allocator and role and quota sorters.
  updateSlaveTotal(slaveId, updatedTotal.get());

  return Nothing();
}


void HierarchicalAllocatorProcess::updateUnavailability(
    const SlaveID& slaveId,
    const Option<Unavailability>& unavailability)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  Slave& slave = slaves.at(slaveId);

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.

  // We explicitly remove all filters for the inverse offers of this slave. We
  // do this because we want to force frameworks to reassess the calculations
  // they have made to respond to the inverse offer. Unavailability of a slave
  // can have a large effect on failure domain calculations and inter-leaved
  // unavailability schedules.
  foreachvalue (Framework& framework, frameworks) {
    framework.inverseOfferFilters.erase(slaveId);
  }

  // Remove any old unavailability.
  slave.maintenance = None();

  // If we have a new unavailability.
  if (unavailability.isSome()) {
    slave.maintenance = Slave::Maintenance(unavailability.get());
  }

  allocate(slaveId);
}


void HierarchicalAllocatorProcess::updateInverseOffer(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Option<UnavailableResources>& unavailableResources,
    const Option<InverseOfferStatus>& status,
    const Option<Filters>& filters)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));
  CHECK(slaves.contains(slaveId));

  Framework& framework = frameworks.at(frameworkId);
  Slave& slave = slaves.at(slaveId);

  CHECK(slave.maintenance.isSome());

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.

  // We use a reference by alias because we intend to modify the
  // `maintenance` and to improve readability.
  Slave::Maintenance& maintenance = slave.maintenance.get();

  // Only handle inverse offers that we currently have outstanding. If it is not
  // currently outstanding this means it is old and can be safely ignored.
  if (maintenance.offersOutstanding.contains(frameworkId)) {
    // We always remove the outstanding offer so that we will send a new offer
    // out the next time we schedule inverse offers.
    maintenance.offersOutstanding.erase(frameworkId);

    // If the response is `Some`, this means the framework responded. Otherwise
    // if it is `None` the inverse offer timed out or was rescinded.
    if (status.isSome()) {
      // For now we don't allow frameworks to respond with `UNKNOWN`. The caller
      // should guard against this. This goes against the pattern of not
      // checking external invariants; however, the allocator and master are
      // currently so tightly coupled that this check is valuable.
      CHECK_NE(status.get().status(), InverseOfferStatus::UNKNOWN);

      // If the framework responded, we update our state to match.
      maintenance.statuses[frameworkId].CopyFrom(status.get());
    }
  }

  // No need to install filters if `filters` is none.
  if (filters.isNone()) {
    return;
  }

  // Create a refused resource filter.
  Try<Duration> seconds = Duration::create(filters.get().refuse_seconds());

  if (seconds.isError()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused inverse offer filter because the input value "
                 << "is invalid: " << seconds.error();

    seconds = Duration::create(Filters().refuse_seconds());
  } else if (seconds.get() < Duration::zero()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused inverse offer filter because the input value "
                 << "is negative";

    seconds = Duration::create(Filters().refuse_seconds());
  }

  CHECK_SOME(seconds);

  if (seconds.get() != Duration::zero()) {
    VLOG(1) << "Framework " << frameworkId
            << " filtered inverse offers from agent " << slaveId
            << " for " << seconds.get();

    // Create a new inverse offer filter and delay its expiration.
    InverseOfferFilter* inverseOfferFilter =
      new RefusedInverseOfferFilter(Timeout::in(seconds.get()));

    framework.inverseOfferFilters[slaveId].insert(inverseOfferFilter);

    // We need to disambiguate the function call to pick the correct
    // `expire()` overload.
    void (Self::*expireInverseOffer)(
             const FrameworkID&,
             const SlaveID&,
             InverseOfferFilter*) = &Self::expire;

    delay(
        seconds.get(),
        self(),
        expireInverseOffer,
        frameworkId,
        slaveId,
        inverseOfferFilter);
  }
}


Future<hashmap<SlaveID, hashmap<FrameworkID, InverseOfferStatus>>>
HierarchicalAllocatorProcess::getInverseOfferStatuses()
{
  CHECK(initialized);

  hashmap<SlaveID, hashmap<FrameworkID, InverseOfferStatus>> result;

  // Make a copy of the most recent statuses.
  foreachpair (const SlaveID& id, const Slave& slave, slaves) {
    if (slave.maintenance.isSome()) {
      result[id] = slave.maintenance.get().statuses;
    }
  }

  return result;
}


void HierarchicalAllocatorProcess::recoverResources(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters)
{
  CHECK(initialized);

  if (resources.empty()) {
    return;
  }

  // For now, we require that resources are recovered within a single
  // allocation role (since filtering in the same manner across roles
  // seems undesirable).
  //
  // TODO(bmahler): The use of `Resources::allocations()` induces
  // unnecessary copying of `Resources` objects (which is expensive
  // at the time this was written).
  hashmap<string, Resources> allocations = resources.allocations();

  CHECK_EQ(1u, allocations.size());

  string role = allocations.begin()->first;

  // Updated resources allocated to framework (if framework still
  // exists, which it might not in the event that we dispatched
  // Master::offer before we received
  // MesosAllocatorProcess::removeFramework or
  // MesosAllocatorProcess::deactivateFramework, in which case we will
  // have already recovered all of its resources).
  if (frameworks.contains(frameworkId)) {
    CHECK(frameworkSorters.contains(role));

    const Owned<Sorter>& frameworkSorter = frameworkSorters.at(role);

    if (frameworkSorter->contains(frameworkId.value())) {
      frameworkSorter->unallocated(frameworkId.value(), slaveId, resources);
      frameworkSorter->remove(slaveId, resources);
      roleSorter->unallocated(role, slaveId, resources);

      if (quotas.contains(role)) {
        // See comment at `quotaRoleSorter` declaration
        // regarding non-revocable
        quotaRoleSorter->unallocated(
            role, slaveId, resources.nonRevocable());
      }

      // Stop tracking the framework under this role if it's no longer
      // subscribed and no longer has resources allocated to the role.
      if (frameworks.at(frameworkId).roles.count(role) == 0 &&
          frameworkSorter->allocation(frameworkId.value()).empty()) {
        untrackFrameworkUnderRole(frameworkId, role);
      }
    }
  }

  // Update resources allocated on slave (if slave still exists,
  // which it might not in the event that we dispatched Master::offer
  // before we received Allocator::removeSlave).
  if (slaves.contains(slaveId)) {
    Slave& slave = slaves.at(slaveId);

    CHECK(slave.allocated.contains(resources));

    slave.allocated -= resources;

    VLOG(1) << "Recovered " << resources
            << " (total: " << slave.total
            << ", allocated: " << slave.allocated << ")"
            << " on agent " << slaveId
            << " from framework " << frameworkId;
  }

  // No need to install the filter if 'filters' is none.
  if (filters.isNone()) {
    return;
  }

  // No need to install the filter if slave/framework does not exist.
  if (!frameworks.contains(frameworkId) || !slaves.contains(slaveId)) {
    return;
  }

  // Create a refused resources filter.
  Try<Duration> timeout = Duration::create(filters.get().refuse_seconds());

  if (timeout.isError()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused resources filter because the input value "
                 << "is invalid: " << timeout.error();

    timeout = Duration::create(Filters().refuse_seconds());
  } else if (timeout.get() < Duration::zero()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused resources filter because the input value "
                 << "is negative";

    timeout = Duration::create(Filters().refuse_seconds());
  }

  CHECK_SOME(timeout);

  if (timeout.get() != Duration::zero()) {
    VLOG(1) << "Framework " << frameworkId
            << " filtered agent " << slaveId
            << " for " << timeout.get();

    // Create a new filter. Note that we unallocate the resources
    // since filters are applied per-role already.
    Resources unallocated = resources;
    unallocated.unallocate();

    OfferFilter* offerFilter = new RefusedOfferFilter(unallocated);
    frameworks.at(frameworkId)
      .offerFilters[role][slaveId].insert(offerFilter);

    // Expire the filter after both an `allocationInterval` and the
    // `timeout` have elapsed. This ensures that the filter does not
    // expire before we perform the next allocation for this agent,
    // see MESOS-4302 for more information.
    //
    // Because the next batched allocation goes through a dispatch
    // after `allocationInterval`, we do the same for `expire()`
    // (with a hepler `_expire()`) to achieve the above.
    //
    // TODO(alexr): If we allocated upon resource recovery
    // (MESOS-3078), we would not need to increase the timeout here.
    timeout = std::max(allocationInterval, timeout.get());

    // We need to disambiguate the function call to pick the correct
    // `expire()` overload.
    void (Self::*expireOffer)(
        const FrameworkID&,
        const string&,
        const SlaveID&,
        OfferFilter*) = &Self::expire;

    delay(timeout.get(),
          self(),
          expireOffer,
          frameworkId,
          role,
          slaveId,
          offerFilter);
  }
}


void HierarchicalAllocatorProcess::suppressOffers(
    const FrameworkID& frameworkId,
    const set<string>& roles_)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  Framework& framework = frameworks.at(frameworkId);

  // Deactivating the framework in the sorter is fine as long as
  // SUPPRESS is not parameterized. When parameterization is added,
  // we have to differentiate between the cases here.
  const set<string>& roles = roles_.empty() ? framework.roles : roles_;

  foreach (const string& role, roles) {
    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->deactivate(frameworkId.value());
  }

  LOG(INFO) << "Suppressed offers for roles " << stringify(roles)
            << " of framework " << frameworkId;
}


void HierarchicalAllocatorProcess::reviveOffers(
    const FrameworkID& frameworkId,
    const set<string>& roles_)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  Framework& framework = frameworks.at(frameworkId);
  framework.offerFilters.clear();
  framework.inverseOfferFilters.clear();

  const set<string>& roles = roles_.empty() ? framework.roles : roles_;

  // Activating the framework in the sorter on REVIVE is fine as long as
  // SUPPRESS is not parameterized. When parameterization is added,
  // we may need to differentiate between the cases here.
  foreach (const string& role, roles) {
    CHECK(frameworkSorters.contains(role));
    frameworkSorters.at(role)->activate(frameworkId.value());
  }

  // We delete each actual `OfferFilter` when
  // `HierarchicalAllocatorProcess::expire` gets invoked. If we delete the
  // `OfferFilter` here it's possible that the same `OfferFilter` (i.e., same
  // address) could get reused and `HierarchicalAllocatorProcess::expire`
  // would expire that filter too soon. Note that this only works
  // right now because ALL Filter types "expire".

  LOG(INFO) << "Revived offers for roles " << stringify(roles)
            << " of framework " << frameworkId;

  allocate();
}


void HierarchicalAllocatorProcess::setQuota(
    const string& role,
    const Quota& quota)
{
  CHECK(initialized);

  // This method should be called by the master only if the quota for
  // the role is not set. Setting quota differs from updating it because
  // the former moves the role to a different allocation group with a
  // dedicated sorter, while the later just updates the actual quota.
  CHECK(!quotas.contains(role));

  // Persist quota in memory and add the role into the corresponding
  // allocation group.
  quotas[role] = quota;
  quotaRoleSorter->add(role);
  quotaRoleSorter->activate(role);

  // Copy allocation information for the quota'ed role.
  if (roleSorter->contains(role)) {
    hashmap<SlaveID, Resources> roleAllocation = roleSorter->allocation(role);
    foreachpair (
        const SlaveID& slaveId, const Resources& resources, roleAllocation) {
      // See comment at `quotaRoleSorter` declaration regarding non-revocable.
      quotaRoleSorter->allocated(role, slaveId, resources.nonRevocable());
    }
  }

  metrics.setQuota(role, quota);

  // TODO(alexr): Print all quota info for the role.
  LOG(INFO) << "Set quota " << quota.info.guarantee() << " for role '" << role
            << "'";

  // NOTE: Since quota changes do not result in rebalancing of
  // offered resources, we do not trigger an allocation here; the
  // quota change will be reflected in subsequent allocations.
  //
  // If we add the ability for quota changes to incur a rebalancing
  // of offered resources, then we should trigger that here.
}


void HierarchicalAllocatorProcess::removeQuota(
    const string& role)
{
  CHECK(initialized);

  // Do not allow removing quota if it is not set.
  CHECK(quotas.contains(role));
  CHECK(quotaRoleSorter->contains(role));

  // TODO(alexr): Print all quota info for the role.
  LOG(INFO) << "Removed quota " << quotas[role].info.guarantee()
            << " for role '" << role << "'";

  // Remove the role from the quota'ed allocation group.
  quotas.erase(role);
  quotaRoleSorter->remove(role);

  metrics.removeQuota(role);

  // NOTE: Since quota changes do not result in rebalancing of
  // offered resources, we do not trigger an allocation here; the
  // quota change will be reflected in subsequent allocations.
  //
  // If we add the ability for quota changes to incur a rebalancing
  // of offered resources, then we should trigger that here.
}


void HierarchicalAllocatorProcess::updateWeights(
    const vector<WeightInfo>& weightInfos)
{
  CHECK(initialized);

  foreach (const WeightInfo& weightInfo, weightInfos) {
    CHECK(weightInfo.has_role());

    quotaRoleSorter->updateWeight(weightInfo.role(), weightInfo.weight());
    roleSorter->updateWeight(weightInfo.role(), weightInfo.weight());
  }

  // NOTE: Since weight changes do not result in rebalancing of
  // offered resources, we do not trigger an allocation here; the
  // weight change will be reflected in subsequent allocations.
  //
  // If we add the ability for weight changes to incur a rebalancing
  // of offered resources, then we should trigger that here.
}


void HierarchicalAllocatorProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Allocation paused";

    paused = true;
  }
}


void HierarchicalAllocatorProcess::resume()
{
  if (paused) {
    VLOG(1) << "Allocation resumed";

    paused = false;
  }
}


void HierarchicalAllocatorProcess::batch()
{
  process::PID<HierarchicalAllocatorProcess> pid = self();
  Duration _allocationInterval = allocationInterval;

  allocate()
    .onAny([_allocationInterval, pid]() {
      delay(_allocationInterval, pid, &HierarchicalAllocatorProcess::batch);
    });
}


Future<Nothing> HierarchicalAllocatorProcess::allocate()
{
  return allocate(slaves.keys());
}


Future<Nothing> HierarchicalAllocatorProcess::allocate(
    const SlaveID& slaveId)
{
  hashset<SlaveID> slaves({slaveId});
  return allocate(slaves);
}


Future<Nothing> HierarchicalAllocatorProcess::allocate(
    const hashset<SlaveID>& slaveIds)
{
  if (paused) {
    VLOG(1) << "Skipped allocation because the allocator is paused";

    return Nothing();
  }

  allocationCandidates |= slaveIds;

  if (allocation.isNone() || !allocation->isPending()) {
    allocation = dispatch(self(), &Self::_allocate);
  }

  return allocation.get();
}


Nothing HierarchicalAllocatorProcess::_allocate() {
  if (paused) {
    VLOG(1) << "Skipped allocation because the allocator is paused";

    return Nothing();
  }

  ++metrics.allocation_runs;

  Stopwatch stopwatch;
  stopwatch.start();
  metrics.allocation_run.start();

  __allocate();

  // NOTE: For now, we implement maintenance inverse offers within the
  // allocator. We leverage the existing timer/cycle of offers to also do any
  // "deallocation" (inverse offers) necessary to satisfy maintenance needs.
  deallocate();

  metrics.allocation_run.stop();

  VLOG(1) << "Performed allocation for " << allocationCandidates.size()
          << " agents in " << stopwatch.elapsed();

  // Clear the candidates on completion of the allocation run.
  allocationCandidates.clear();

  return Nothing();
}


// TODO(alexr): Consider factoring out the quota allocation logic.
void HierarchicalAllocatorProcess::__allocate()
{
  // Compute the offerable resources, per framework:
  //   (1) For reserved resources on the slave, allocate these to a
  //       framework having the corresponding role.
  //   (2) For unreserved resources on the slave, allocate these
  //       to a framework of any role.
  hashmap<FrameworkID, hashmap<string, hashmap<SlaveID, Resources>>> offerable;

  // NOTE: This function can operate on a small subset of
  // `allocationCandidates`, we have to make sure that we don't
  // assume cluster knowledge when summing resources from that set.

  vector<SlaveID> slaveIds;
  slaveIds.reserve(allocationCandidates.size());

  // Filter out non-whitelisted, removed, and deactivated slaves
  // in order not to send offers for them.
  foreach (const SlaveID& slaveId, allocationCandidates) {
    if (isWhitelisted(slaveId) &&
        slaves.contains(slaveId) &&
        slaves.at(slaveId).activated) {
      slaveIds.push_back(slaveId);
    }
  }

  // Randomize the order in which slaves' resources are allocated.
  //
  // TODO(vinod): Implement a smarter sorting algorithm.
  std::random_shuffle(slaveIds.begin(), slaveIds.end());

  // Returns the __quantity__ of resources allocated to a quota role. Since we
  // account for reservations and persistent volumes toward quota, we strip
  // reservation and persistent volume related information for comparability.
  // The result is used to determine whether a role's quota is satisfied, and
  // also to determine how many resources the role would need in order to meet
  // its quota.
  //
  // NOTE: Revocable resources are excluded in `quotaRoleSorter`.
  auto getQuotaRoleAllocatedResources = [this](const string& role) {
    CHECK(quotas.contains(role));

    // NOTE: `allocationScalarQuantities` omits dynamic reservation,
    // persistent volume info, and allocation info. We additionally
    // strip the `Resource.role` here via `flatten()`.
    return quotaRoleSorter->allocationScalarQuantities(role).flatten();
  };

  // Due to the two stages in the allocation algorithm and the nature of
  // shared resources being re-offerable even if already allocated, the
  // same shared resources can appear in two (and not more due to the
  // `allocatable` check in each stage) distinct offers in one allocation
  // cycle. This is undesirable since the allocator API contract should
  // not depend on its implementation details. For now we make sure a
  // shared resource is only allocated once in one offer cycle. We use
  // `offeredSharedResources` to keep track of shared resources already
  // allocated in the current cycle.
  hashmap<SlaveID, Resources> offeredSharedResources;

  // Quota comes first and fair share second. Here we process only those
  // roles for which quota is set (quota'ed roles). Such roles form a
  // special allocation group with a dedicated sorter.
  foreach (const SlaveID& slaveId, slaveIds) {
    foreach (const string& role, quotaRoleSorter->sort()) {
      CHECK(quotas.contains(role));

      const Quota& quota = quotas.at(role);

      // If there are no active frameworks in this role, we do not
      // need to do any allocations for this role.
      if (!roles.contains(role)) {
        continue;
      }

      // Get the total quantity of resources allocated to a quota role. The
      // value omits role, reservation, and persistence info.
      Resources roleConsumedResources = getQuotaRoleAllocatedResources(role);

      // If quota for the role is satisfied, we do not need to do
      // any further allocations for this role, at least at this
      // stage. More precisely, we stop allocating if at least
      // one of the resource guarantees is met. This technique
      // prevents gaming of the quota allocation, see MESOS-6432.
      //
      // Longer term, we could ideally allocate what remains
      // unsatisfied to allow an existing container to scale
      // vertically, or to allow the launching of a container
      // with best-effort cpus/mem/disk/etc.
      //
      // TODO(alexr): Skipping satisfied roles is pessimistic. Better
      // alternatives are:
      //   * A custom sorter that is aware of quotas and sorts accordingly.
      //   * Removing satisfied roles from the sorter.
      bool someGuaranteesReached = false;
      foreach (const Resource& guarantee, quota.info.guarantee()) {
        if (roleConsumedResources.contains(guarantee)) {
          someGuaranteesReached = true;
          break;
        }
      }

      if (someGuaranteesReached) {
        continue;
      }

      // Fetch frameworks according to their fair share.
      // NOTE: Suppressed frameworks are not included in the sort.
      CHECK(frameworkSorters.contains(role));
      const Owned<Sorter>& frameworkSorter = frameworkSorters.at(role);

      foreach (const string& frameworkId_, frameworkSorter->sort()) {
        FrameworkID frameworkId;
        frameworkId.set_value(frameworkId_);

        CHECK(slaves.contains(slaveId));
        CHECK(frameworks.contains(frameworkId));

        const Framework& framework = frameworks.at(frameworkId);
        Slave& slave = slaves.at(slaveId);

        // Only offer resources from slaves that have GPUs to
        // frameworks that are capable of receiving GPUs.
        // See MESOS-5634.
        if (!framework.capabilities.gpuResources &&
            slave.total.gpus().getOrElse(0) > 0) {
          continue;
        }

        // Calculate the currently available resources on the slave, which
        // is the difference in non-shared resources between total and
        // allocated, plus all shared resources on the agent (if applicable).
        // Since shared resources are offerable even when they are in use, we
        // make one copy of the shared resources available regardless of the
        // past allocations.
        Resources available = slave.available().nonShared();

        // Offer a shared resource only if it has not been offered in
        // this offer cycle to a framework.
        if (framework.capabilities.sharedResources) {
          available += slave.total.shared();
          if (offeredSharedResources.contains(slaveId)) {
            available -= offeredSharedResources[slaveId];
          }
        }

        // The resources we offer are the unreserved resources as well as the
        // reserved resources for this particular role. This is necessary to
        // ensure that we don't offer resources that are reserved for another
        // role.
        //
        // NOTE: Currently, frameworks are allowed to have '*' role.
        // Calling reserved('*') returns an empty Resources object.
        //
        // Quota is satisfied from the available non-revocable resources on the
        // agent. It's important that we include reserved resources here since
        // reserved resources are accounted towards the quota guarantee. If we
        // were to rely on stage 2 to offer them out, they would not be checked
        // against the quota guarantee.
        Resources resources =
          (available.unreserved() + available.reserved(role)).nonRevocable();

        // It is safe to break here, because all frameworks under a role would
        // consider the same resources, so in case we don't have allocatable
        // resources, we don't have to check for other frameworks under the
        // same role. We only break out of the innermost loop, so the next step
        // will use the same `slaveId`, but a different role.
        //
        // NOTE: The resources may not be allocatable here, but they can be
        // accepted by one of the frameworks during the second allocation
        // stage.
        if (!allocatable(resources)) {
          break;
        }

        // If the framework filters these resources, ignore. The unallocated
        // part of the quota will not be allocated to other roles.
        if (isFiltered(frameworkId, role, slaveId, resources)) {
          continue;
        }

        VLOG(2) << "Allocating " << resources << " on agent " << slaveId
                << " to role " << role << " of framework " << frameworkId
                << " as part of its role quota";

        resources.allocate(role);

        // NOTE: We perform "coarse-grained" allocation for quota'ed
        // resources, which may lead to overcommitment of resources beyond
        // quota. This is fine since quota currently represents a guarantee.
        offerable[frameworkId][role][slaveId] += resources;
        offeredSharedResources[slaveId] += resources.shared();

        slave.allocated += resources;

        // Resources allocated as part of the quota count towards the
        // role's and the framework's fair share.
        //
        // NOTE: Revocable resources have already been excluded.
        frameworkSorter->add(slaveId, resources);
        frameworkSorter->allocated(frameworkId_, slaveId, resources);
        roleSorter->allocated(role, slaveId, resources);
        quotaRoleSorter->allocated(role, slaveId, resources);
      }
    }
  }

  // Calculate the total quantity of scalar resources (including revocable
  // and reserved) that are available for allocation in the next round. We
  // need this in order to ensure we do not over-allocate resources during
  // the second stage.
  //
  // For performance reasons (MESOS-4833), this omits information about
  // dynamic reservations or persistent volumes in the resources.
  //
  // NOTE: We use total cluster resources, and not just those based on the
  // agents participating in the current allocation (i.e. provided as an
  // argument to the `allocate()` call) so that frameworks in roles without
  // quota are not unnecessarily deprived of resources.
  Resources remainingClusterResources = roleSorter->totalScalarQuantities();
  foreachkey (const string& role, roles) {
    remainingClusterResources -= roleSorter->allocationScalarQuantities(role);
  }

  // Frameworks in a quota'ed role may temporarily reject resources by
  // filtering or suppressing offers. Hence quotas may not be fully allocated.
  Resources unallocatedQuotaResources;
  foreachpair (const string& name, const Quota& quota, quotas) {
    // Compute the amount of quota that the role does not have allocated.
    //
    // NOTE: Revocable resources are excluded in `quotaRoleSorter`.
    // NOTE: Only scalars are considered for quota.
    Resources allocated = getQuotaRoleAllocatedResources(name);
    const Resources required = quota.info.guarantee();
    unallocatedQuotaResources += (required - allocated);
  }

  // Determine how many resources we may allocate during the next stage.
  //
  // NOTE: Resources for quota allocations are already accounted in
  // `remainingClusterResources`.
  remainingClusterResources -= unallocatedQuotaResources;

  // Shared resources are excluded in determination of over-allocation of
  // available resources since shared resources are always allocatable.
  remainingClusterResources = remainingClusterResources.nonShared();

  // To ensure we do not over-allocate resources during the second stage
  // with all frameworks, we use 2 stopping criteria:
  //   * No available resources for the second stage left, i.e.
  //     `remainingClusterResources` - `allocatedStage2` is empty.
  //   * A potential offer will force the second stage to use more resources
  //     than available, i.e. `remainingClusterResources` does not contain
  //     (`allocatedStage2` + potential offer). In this case we skip this
  //     agent and continue to the next one.
  //
  // NOTE: Like `remainingClusterResources`, `allocatedStage2` omits
  // information about dynamic reservations and persistent volumes for
  // performance reasons. This invariant is preserved because we only add
  // resources to it that have also had this metadata stripped from them
  // (typically by using `Resources::createStrippedScalarQuantity`).
  Resources allocatedStage2;

  // At this point resources for quotas are allocated or accounted for.
  // Proceed with allocating the remaining free pool.
  foreach (const SlaveID& slaveId, slaveIds) {
    // If there are no resources available for the second stage, stop.
    if (!allocatable(remainingClusterResources - allocatedStage2)) {
      break;
    }

    foreach (const string& role, roleSorter->sort()) {
      // NOTE: Suppressed frameworks are not included in the sort.
      CHECK(frameworkSorters.contains(role));
      const Owned<Sorter>& frameworkSorter = frameworkSorters.at(role);

      foreach (const string& frameworkId_, frameworkSorter->sort()) {
        FrameworkID frameworkId;
        frameworkId.set_value(frameworkId_);

        CHECK(slaves.contains(slaveId));
        CHECK(frameworks.contains(frameworkId));

        const Framework& framework = frameworks.at(frameworkId);
        Slave& slave = slaves.at(slaveId);

        // Only offer resources from slaves that have GPUs to
        // frameworks that are capable of receiving GPUs.
        // See MESOS-5634.
        if (!framework.capabilities.gpuResources &&
            slave.total.gpus().getOrElse(0) > 0) {
          continue;
        }

        // Calculate the currently available resources on the slave, which
        // is the difference in non-shared resources between total and
        // allocated, plus all shared resources on the agent (if applicable).
        // Since shared resources are offerable even when they are in use, we
        // make one copy of the shared resources available regardless of the
        // past allocations.
        Resources available = slave.available().nonShared();

        // Offer a shared resource only if it has not been offered in
        // this offer cycle to a framework.
        if (framework.capabilities.sharedResources) {
          available += slave.total.shared();
          if (offeredSharedResources.contains(slaveId)) {
            available -= offeredSharedResources[slaveId];
          }
        }

        // The resources we offer are the unreserved resources as well as the
        // reserved resources for this particular role. This is necessary to
        // ensure that we don't offer resources that are reserved for another
        // role.
        //
        // NOTE: Currently, frameworks are allowed to have '*' role.
        // Calling reserved('*') returns an empty Resources object.
        //
        // NOTE: We do not offer roles with quota any more non-revocable
        // resources once their quota is satisfied. However, note that this is
        // not strictly true due to the coarse-grained nature (per agent) of the
        // allocation algorithm in stage 1.
        //
        // TODO(mpark): Offer unreserved resources as revocable beyond quota.
        Resources resources = available.reserved(role);
        if (!quotas.contains(role)) {
          resources += available.unreserved();
        }

        // It is safe to break here, because all frameworks under a role would
        // consider the same resources, so in case we don't have allocatable
        // resources, we don't have to check for other frameworks under the
        // same role. We only break out of the innermost loop, so the next step
        // will use the same slaveId, but a different role.
        //
        // The difference to the second `allocatable` check is that here we also
        // check for revocable resources, which can be disabled on a per frame-
        // work basis, which requires us to go through all frameworks in case we
        // have allocatable revocable resources.
        if (!allocatable(resources)) {
          break;
        }

        // Remove revocable resources if the framework has not opted for them.
        if (!framework.capabilities.revocableResources) {
          resources = resources.nonRevocable();
        }

        // If the resources are not allocatable, ignore. We cannot break
        // here, because another framework under the same role could accept
        // revocable resources and breaking would skip all other frameworks.
        if (!allocatable(resources)) {
          continue;
        }

        // If the framework filters these resources, ignore.
        if (isFiltered(frameworkId, role, slaveId, resources)) {
          continue;
        }

        // If the offer generated by `resources` would force the second
        // stage to use more than `remainingClusterResources`, move along.
        // We do not terminate early, as offers generated further in the
        // loop may be small enough to fit within `remainingClusterResources`.
        //
        // We exclude shared resources from over-allocation check because
        // shared resources are always allocatable.
        const Resources scalarQuantity =
          resources.nonShared().createStrippedScalarQuantity();

        if (!remainingClusterResources.contains(
                allocatedStage2 + scalarQuantity)) {
          continue;
        }

        VLOG(2) << "Allocating " << resources << " on agent " << slaveId
                << " to role " << role << " of framework " << frameworkId;

        resources.allocate(role);

        // NOTE: We perform "coarse-grained" allocation, meaning that we always
        // allocate the entire remaining slave resources to a single framework.
        //
        // NOTE: We may have already allocated some resources on the current
        // agent as part of quota.
        offerable[frameworkId][role][slaveId] += resources;
        offeredSharedResources[slaveId] += resources.shared();
        allocatedStage2 += scalarQuantity;

        slave.allocated += resources;

        frameworkSorter->add(slaveId, resources);
        frameworkSorter->allocated(frameworkId_, slaveId, resources);
        roleSorter->allocated(role, slaveId, resources);

        if (quotas.contains(role)) {
          // See comment at `quotaRoleSorter` declaration regarding
          // non-revocable.
          quotaRoleSorter->allocated(role, slaveId, resources.nonRevocable());
        }
      }
    }
  }

  if (offerable.empty()) {
    VLOG(1) << "No allocations performed";
  } else {
    // Now offer the resources to each framework.
    foreachkey (const FrameworkID& frameworkId, offerable) {
      offerCallback(frameworkId, offerable.at(frameworkId));
    }
  }
}


void HierarchicalAllocatorProcess::deallocate()
{
  // If no frameworks are currently registered, no work to do.
  if (roles.empty()) {
    return;
  }
  CHECK(!frameworkSorters.empty());

  // In this case, `offerable` is actually the slaves and/or resources that we
  // want the master to create `InverseOffer`s from.
  hashmap<FrameworkID, hashmap<SlaveID, UnavailableResources>> offerable;

  // For maintenance, we use the framework sorters to determine which frameworks
  // have (1) reserved and / or (2) unreserved resource on the specified
  // slaveIds. This way we only send inverse offers to frameworks that have the
  // potential to lose something. We keep track of which frameworks already have
  // an outstanding inverse offer for the given slave in the
  // UnavailabilityStatus of the specific slave using the `offerOutstanding`
  // flag. This is equivalent to the accounting we do for resources when we send
  // regular offers. If we didn't keep track of outstanding offers then we would
  // keep generating new inverse offers even though the framework had not
  // responded yet.

  foreachvalue (const Owned<Sorter>& frameworkSorter, frameworkSorters) {
    foreach (const SlaveID& slaveId, allocationCandidates) {
      CHECK(slaves.contains(slaveId));

      Slave& slave = slaves.at(slaveId);

      if (slave.maintenance.isSome()) {
        // We use a reference by alias because we intend to modify the
        // `maintenance` and to improve readability.
        Slave::Maintenance& maintenance = slave.maintenance.get();

        hashmap<string, Resources> allocation =
          frameworkSorter->allocation(slaveId);

        foreachkey (const string& frameworkId_, allocation) {
          FrameworkID frameworkId;
          frameworkId.set_value(frameworkId_);

          // If this framework doesn't already have inverse offers for the
          // specified slave.
          if (!offerable[frameworkId].contains(slaveId)) {
            // If there isn't already an outstanding inverse offer to this
            // framework for the specified slave.
            if (!maintenance.offersOutstanding.contains(frameworkId)) {
              // Ignore in case the framework filters inverse offers for this
              // slave.
              //
              // NOTE: Since this specific allocator implementation only sends
              // inverse offers for maintenance primitives, and those are at the
              // whole slave level, we only need to filter based on the
              // time-out.
              if (isFiltered(frameworkId, slaveId)) {
                continue;
              }

              const UnavailableResources unavailableResources =
                UnavailableResources{
                    Resources(),
                    maintenance.unavailability};

              // For now we send inverse offers with empty resources when the
              // inverse offer represents maintenance on the machine. In the
              // future we could be more specific about the resources on the
              // host, as we have the information available.
              offerable[frameworkId][slaveId] = unavailableResources;

              // Mark this framework as having an offer outstanding for the
              // specified slave.
              maintenance.offersOutstanding.insert(frameworkId);
            }
          }
        }
      }
    }
  }

  if (offerable.empty()) {
    VLOG(1) << "No inverse offers to send out!";
  } else {
    // Now send inverse offers to each framework.
    foreachkey (const FrameworkID& frameworkId, offerable) {
      inverseOfferCallback(frameworkId, offerable[frameworkId]);
    }
  }
}


void HierarchicalAllocatorProcess::_expire(
    const FrameworkID& frameworkId,
    const string& role,
    const SlaveID& slaveId,
    OfferFilter* offerFilter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in `reviveOffers()`) but not
  // yet deleted (to keep the address from getting reused
  // possibly causing premature expiration).
  //
  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.

  auto frameworkIterator = frameworks.find(frameworkId);
  if (frameworkIterator != frameworks.end()) {
    Framework& framework = frameworkIterator->second;

    auto roleFilters = framework.offerFilters.find(role);
    if (roleFilters != framework.offerFilters.end()) {
      auto agentFilters = roleFilters->second.find(slaveId);

      if (agentFilters != roleFilters->second.end()) {
        // Erase the filter (may be a no-op per the comment above).
        agentFilters->second.erase(offerFilter);

        if (agentFilters->second.empty()) {
          roleFilters->second.erase(slaveId);
        }
      }
    }
  }

  delete offerFilter;
}


void HierarchicalAllocatorProcess::expire(
    const FrameworkID& frameworkId,
    const string& role,
    const SlaveID& slaveId,
    OfferFilter* offerFilter)
{
  dispatch(
      self(),
      &Self::_expire,
      frameworkId,
      role,
      slaveId,
      offerFilter);
}


void HierarchicalAllocatorProcess::expire(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    InverseOfferFilter* inverseOfferFilter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in
  // HierarchicalAllocatorProcess::reviveOffers) but not yet deleted (to
  // keep the address from getting reused possibly causing premature
  // expiration).
  //
  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.

  auto frameworkIterator = frameworks.find(frameworkId);
  if (frameworkIterator != frameworks.end()) {
    Framework& framework = frameworkIterator->second;

    auto filters = framework.inverseOfferFilters.find(slaveId);
    if (filters != framework.inverseOfferFilters.end()) {
      filters->second.erase(inverseOfferFilter);

      if (filters->second.empty()) {
        framework.inverseOfferFilters.erase(slaveId);
      }
    }
  }

  delete inverseOfferFilter;
}


bool HierarchicalAllocatorProcess::isWhitelisted(
    const SlaveID& slaveId) const
{
  CHECK(slaves.contains(slaveId));

  const Slave& slave = slaves.at(slaveId);

  return whitelist.isNone() || whitelist->contains(slave.hostname);
}


bool HierarchicalAllocatorProcess::isFiltered(
    const FrameworkID& frameworkId,
    const string& role,
    const SlaveID& slaveId,
    const Resources& resources) const
{
  CHECK(frameworks.contains(frameworkId));
  CHECK(slaves.contains(slaveId));

  const Framework& framework = frameworks.at(frameworkId);
  const Slave& slave = slaves.at(slaveId);

  // Prevent offers from non-MULTI_ROLE agents to be allocated
  // to MULTI_ROLE frameworks.
  if (framework.capabilities.multiRole &&
      !slave.capabilities.multiRole) {
    LOG(WARNING)
      << "Implicitly filtering agent " << slaveId << " from framework"
      << frameworkId << " because the framework is MULTI_ROLE capable"
      << " but the agent is not";

    return true;
  }

  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.
  auto roleFilters = framework.offerFilters.find(role);
  if (roleFilters == framework.offerFilters.end()) {
    return false;
  }

  auto agentFilters = roleFilters->second.find(slaveId);
  if (agentFilters == roleFilters->second.end()) {
    return false;
  }

  foreach (OfferFilter* offerFilter, agentFilters->second) {
    if (offerFilter->filter(resources)) {
      VLOG(1) << "Filtered offer with " << resources
              << " on agent " << slaveId
              << " for role " << role
              << " of framework " << frameworkId;

      return true;
    }
  }

  return false;
}


bool HierarchicalAllocatorProcess::isFiltered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId) const
{
  CHECK(frameworks.contains(frameworkId));
  CHECK(slaves.contains(slaveId));

  const Framework& framework = frameworks.at(frameworkId);

  if (framework.inverseOfferFilters.contains(slaveId)) {
    foreach (InverseOfferFilter* inverseOfferFilter,
             framework.inverseOfferFilters.at(slaveId)) {
      if (inverseOfferFilter->filter()) {
        VLOG(1) << "Filtered unavailability on agent " << slaveId
                << " for framework " << frameworkId;

        return true;
      }
    }
  }

  return false;
}


bool HierarchicalAllocatorProcess::allocatable(
    const Resources& resources)
{
  Option<double> cpus = resources.cpus();
  Option<Bytes> mem = resources.mem();

  return (cpus.isSome() && cpus.get() >= MIN_CPUS) ||
         (mem.isSome() && mem.get() >= MIN_MEM);
}


double HierarchicalAllocatorProcess::_resources_offered_or_allocated(
    const string& resource)
{
  double offered_or_allocated = 0;

  foreachvalue (const Slave& slave, slaves) {
    Option<Value::Scalar> value =
      slave.allocated.get<Value::Scalar>(resource);

    if (value.isSome()) {
      offered_or_allocated += value->value();
    }
  }

  return offered_or_allocated;
}


double HierarchicalAllocatorProcess::_resources_total(
    const string& resource)
{
  Option<Value::Scalar> total =
    roleSorter->totalScalarQuantities()
      .get<Value::Scalar>(resource);

  return total.isSome() ? total->value() : 0;
}


double HierarchicalAllocatorProcess::_quota_allocated(
    const string& role,
    const string& resource)
{
  Option<Value::Scalar> used =
    quotaRoleSorter->allocationScalarQuantities(role)
      .get<Value::Scalar>(resource);

  return used.isSome() ? used->value() : 0;
}


double HierarchicalAllocatorProcess::_offer_filters_active(
    const string& role)
{
  double result = 0;

  foreachvalue (const Framework& framework, frameworks) {
    if (!framework.offerFilters.contains(role)) {
      continue;
    }

    foreachkey (const SlaveID& slaveId, framework.offerFilters.at(role)) {
      result += framework.offerFilters.at(role).at(slaveId).size();
    }
  }

  return result;
}


bool HierarchicalAllocatorProcess::isFrameworkTrackedUnderRole(
    const FrameworkID& frameworkId,
    const string& role) const
{
  return roles.contains(role) &&
         roles.at(role).contains(frameworkId);
}


void HierarchicalAllocatorProcess::trackFrameworkUnderRole(
    const FrameworkID& frameworkId,
    const string& role)
{
  CHECK(initialized);

  // If this is the first framework to subscribe to this role, or have
  // resources allocated to this role, initialize state as necessary.
  if (!roles.contains(role)) {
    roles[role] = {};
    CHECK(!roleSorter->contains(role));
    roleSorter->add(role);
    roleSorter->activate(role);

    CHECK(!frameworkSorters.contains(role));
    frameworkSorters.insert({role, Owned<Sorter>(frameworkSorterFactory())});
    frameworkSorters.at(role)->initialize(fairnessExcludeResourceNames);
    metrics.addRole(role);
  }

  CHECK(!roles.at(role).contains(frameworkId));
  roles.at(role).insert(frameworkId);

  CHECK(!frameworkSorters.at(role)->contains(frameworkId.value()));
  frameworkSorters.at(role)->add(frameworkId.value());
}


void HierarchicalAllocatorProcess::untrackFrameworkUnderRole(
    const FrameworkID& frameworkId,
    const string& role)
{
  CHECK(initialized);

  CHECK(roles.contains(role));
  CHECK(roles.at(role).contains(frameworkId));
  CHECK(frameworkSorters.contains(role));
  CHECK(frameworkSorters.at(role)->contains(frameworkId.value()));

  roles.at(role).erase(frameworkId);
  frameworkSorters.at(role)->remove(frameworkId.value());

  // If no more frameworks are subscribed to this role or have resources
  // allocated to this role, cleanup associated state. This is not necessary
  // for correctness (roles with no registered frameworks will not be offered
  // any resources), but since many different role names might be used over
  // time, we want to avoid leaking resources for no-longer-used role names.
  // Note that we don't remove the role from `quotaRoleSorter` if it exists
  // there, since roles with a quota set still influence allocation even if
  // they don't have any registered frameworks.

  if (roles.at(role).empty()) {
    CHECK_EQ(frameworkSorters.at(role)->count(), 0);

    roles.erase(role);
    roleSorter->remove(role);

    frameworkSorters.erase(role);

    metrics.removeRole(role);
  }
}


void HierarchicalAllocatorProcess::updateSlaveTotal(
    const SlaveID& slaveId,
    const Resources& total)
{
  CHECK(slaves.contains(slaveId));

  Slave& slave = slaves.at(slaveId);

  const Resources oldTotal = slave.total;
  slave.total = total;

  // Currently `roleSorter` and `quotaRoleSorter`, being the root-level
  // sorters, maintain all of `slaves[slaveId].total` (or the `nonRevocable()`
  // portion in the case of `quotaRoleSorter`) in their own totals (which
  // don't get updated in the allocation runs or during recovery of allocated
  // resources). So, we update them using the resources in `slave.total`.
  roleSorter->remove(slaveId, oldTotal);
  roleSorter->add(slaveId, total);

  // See comment at `quotaRoleSorter` declaration regarding non-revocable.
  quotaRoleSorter->remove(slaveId, oldTotal.nonRevocable());
  quotaRoleSorter->add(slaveId, total.nonRevocable());
}

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
