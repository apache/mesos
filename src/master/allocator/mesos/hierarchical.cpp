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

#include <process/after.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
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

using process::after;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::PID;
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
    const FrameworkInfo& frameworkInfo,
    const set<string>& _suppressedRoles)
  : roles(protobuf::framework::getRoles(frameworkInfo)),
    suppressedRoles(_suppressedRoles),
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
    const Option<set<string>>& _fairnessExcludeResourceNames,
    bool _filterGpuResources,
    const Option<DomainInfo>& _domain,
    const Option<std::vector<Resources>>& _minAllocatableResources)
{
  allocationInterval = _allocationInterval;
  offerCallback = _offerCallback;
  inverseOfferCallback = _inverseOfferCallback;
  fairnessExcludeResourceNames = _fairnessExcludeResourceNames;
  filterGpuResources = _filterGpuResources;
  domain = _domain;
  minAllocatableResources = _minAllocatableResources;
  initialized = true;
  paused = false;

  // Resources for quota'ed roles are allocated separately and prior to
  // non-quota'ed roles, hence a dedicated sorter for quota'ed roles is
  // necessary.
  roleSorter->initialize(fairnessExcludeResourceNames);
  quotaRoleSorter->initialize(fairnessExcludeResourceNames);

  VLOG(1) << "Initialized hierarchical allocator process";

  // Start a loop to run allocation periodically.
  PID<HierarchicalAllocatorProcess> _self = self();

  loop(
      None(), // Use `None` so we iterate outside the allocator process.
      [_allocationInterval]() {
        return after(_allocationInterval);
      },
      [_self](const Nothing&) {
        return dispatch(_self, &HierarchicalAllocatorProcess::allocate)
          .then([]() -> ControlFlow<Nothing> { return Continue(); });
      });
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
    bool active,
    const set<string>& suppressedRoles)
{
  CHECK(initialized);
  CHECK(!frameworks.contains(frameworkId));

  frameworks.insert({frameworkId, Framework(frameworkInfo, suppressedRoles)});

  const Framework& framework = frameworks.at(frameworkId);

  foreach (const string& role, framework.roles) {
    trackFrameworkUnderRole(frameworkId, role);

    CHECK(frameworkSorters.contains(role));

    if (suppressedRoles.count(role)) {
      frameworkSorters.at(role)->deactivate(frameworkId.value());
    } else {
      frameworkSorters.at(role)->activate(frameworkId.value());
    }
  }

  // TODO(bmahler): Validate that the reserved resources have the
  // framework's role.

  // Update the allocation for this framework.
  foreachpair (const SlaveID& slaveId, const Resources& resources, used) {
    // TODO(bmahler): The master won't tell us about resources
    // allocated to agents that have not yet been added, consider
    // CHECKing this case.
    if (!slaves.contains(slaveId)) {
      continue;
    }

    // The slave struct will already be aware of the allocated
    // resources, so we only need to track them in the sorters.
    trackAllocatedResources(slaveId, frameworkId, resources);
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
    //
    // TODO(mzhu): This check may no longer be necessary.
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
      untrackAllocatedResources(slaveId, frameworkId, allocated);
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

  // Activate all roles for this framework except the roles that
  // are marked as deactivated.
  // Note: A subset of framework roles can be deactivated if the
  // role is specified in `suppressed_roles` during framework
  // (re)registration, or via a subsequent `SUPPRESS` call.
  foreach (const string& role, framework.roles) {
    CHECK(frameworkSorters.contains(role));

    if (!framework.suppressedRoles.count(role)) {
      frameworkSorters.at(role)->activate(frameworkId.value());
    }
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
    const FrameworkInfo& frameworkInfo,
    const set<string>& suppressedRoles)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  Framework& framework = frameworks.at(frameworkId);

  set<string> oldRoles = framework.roles;
  set<string> newRoles = protobuf::framework::getRoles(frameworkInfo);
  set<string> oldSuppressedRoles = framework.suppressedRoles;

  // The roles which are candidates for deactivation are the roles that are
  // removed, as well as the roles which have moved from non-suppressed
  // to suppressed mode.
  const set<string> rolesToDeactivate = [&]() {
    set<string> result = oldRoles;
    foreach (const string& role, newRoles) {
      result.erase(role);
    }

    foreach (const string& role, oldRoles) {
      if (!oldSuppressedRoles.count(role) && suppressedRoles.count(role)) {
        result.insert(role);
      }
    }
    return result;
  }();

  foreach (const string& role, rolesToDeactivate) {
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

  // The roles which are candidates for activation are the roles that are
  // added, as well as the roles which have moved from suppressed to
  // non-suppressed mode.
  //
  // TODO(anindya_sinha): We should activate the roles only if the
  // framework is active (instead of always).
  const set<string> rolesToActivate = [&]() {
    set<string> result = newRoles;
    foreach (const string& role, oldRoles) {
      result.erase(role);
    }

    foreach (const string& role, newRoles) {
      if (!suppressedRoles.count(role) && oldSuppressedRoles.count(role)) {
        result.insert(role);
      } else if (suppressedRoles.count(role)) {
        result.erase(role);
      }
    }
    return result;
  }();

  foreach (const string& role, rolesToActivate) {
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
  framework.suppressedRoles = suppressedRoles;
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

  slaves.insert({slaveId,
                 Slave(
                     slaveInfo.hostname(),
                     protobuf::slave::Capabilities(capabilities),
                     true,
                     total,
                     Resources::sum(used))});

  Slave& slave = slaves.at(slaveId);

  if (slaveInfo.has_domain()) {
    slave.domain = slaveInfo.domain();
  }

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.
  if (unavailability.isSome()) {
    slave.maintenance = Slave::Maintenance(unavailability.get());
  }

  trackReservations(total.reservations());

  roleSorter->add(slaveId, total);

  // See comment at `quotaRoleSorter` declaration regarding non-revocable.
  quotaRoleSorter->add(slaveId, total.nonRevocable());

  foreachpair (const FrameworkID& frameworkId,
               const Resources& allocation,
               used) {
    // There are two cases here:
    //
    //   (1) The framework has already been added to the allocator.
    //       In this case, we track the allocation in the sorters.
    //
    //   (2) The framework has not yet been added to the allocator.
    //       The master will imminently add the framework using
    //       the `FrameworkInfo` recovered from the agent, and in
    //       the interim we do not track the resources allocated to
    //       this framework. This leaves a small window where the
    //       role sorting will under-account for the roles belonging
    //       to this framework.
    //
    // TODO(bmahler): Fix the issue outlined in (2).
    if (!frameworks.contains(frameworkId)) {
      continue;
    }

    trackAllocatedResources(slaveId, frameworkId, allocation);
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
            << " with " << slave.getTotal()
            << " (allocated: " << slave.getAllocated() << ")";

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

  roleSorter->remove(slaveId, slaves.at(slaveId).getTotal());

  // See comment at `quotaRoleSorter` declaration regarding non-revocable.
  quotaRoleSorter->remove(
      slaveId, slaves.at(slaveId).getTotal().nonRevocable());

  untrackReservations(slaves.at(slaveId).getTotal().reservations());

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
    const Option<Resources>& total,
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

  if (total.isSome()) {
    updated = updateSlaveTotal(slaveId, total.get());

    LOG(INFO) << "Agent " << slaveId << " (" << slave.hostname << ")"
              << " updated with total resources " << total.get();
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
  slave.unallocate(offeredResources);
  slave.allocate(updatedOfferedResources);

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

  Try<Resources> updatedTotal = slave.getTotal().apply(strippedOperations);
  CHECK_SOME(updatedTotal);
  updateSlaveTotal(slaveId, updatedTotal.get());

  // Update the total resources in the framework sorter.
  frameworkSorter->remove(slaveId, offeredResources);
  frameworkSorter->add(slaveId, updatedOfferedResources);

  // Check that the unreserved quantities for framework allocations
  // have not changed by the above operations.
  const Resources updatedFrameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  CHECK_EQ(
      frameworkAllocation.toUnreserved().createStrippedScalarQuantity(),
      updatedFrameworkAllocation.toUnreserved().createStrippedScalarQuantity());

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
  Try<Resources> updatedAvailable = slave.getAvailable().apply(operations);
  if (updatedAvailable.isError()) {
    return Failure(updatedAvailable.error());
  }

  // Update the total resources.
  Try<Resources> updatedTotal = slave.getTotal().apply(operations);
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
      untrackAllocatedResources(slaveId, frameworkId, resources);

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

    CHECK(slave.getAllocated().contains(resources))
      << slave.getAllocated() << " does not contain " << resources;

    slave.unallocate(resources);

    VLOG(1) << "Recovered " << resources
            << " (total: " << slave.getTotal()
            << ", allocated: " << slave.getAllocated() << ")"
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
    // Because the next periodic allocation goes through a dispatch
    // after `allocationInterval`, we do the same for `expire()`
    // (with a helper `_expire()`) to achieve the above.
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
    framework.suppressedRoles.insert(role);
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
    framework.suppressedRoles.erase(role);
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
    metrics.allocation_run_latency.start();
    allocation = dispatch(self(), &Self::_allocate);
  }

  return allocation.get();
}


Nothing HierarchicalAllocatorProcess::_allocate()
{
  metrics.allocation_run_latency.stop();

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
    // remove the resource's `role` here via `toUnreserved()`.
    return quotaRoleSorter->allocationScalarQuantities(role).toUnreserved();
  };

  // We need to keep track of allocated reserved resourecs for roles
  // with quota in order to enforce their quota limit. Note these are
  // __quantities__ with no meta-data.
  hashmap<string, Resources> allocatedReservationScalarQuantities;

  // We build the map here to avoid repetitive aggregation
  // in the allocation loop. Note, this map will still need to be
  // updated during the allocation loop when new allocations
  // are made.
  //
  // TODO(mzhu): Ideally, we want to buildup and persist this information
  // across allocation cycles in track/untrackAllocatedResources().
  // But due to the presence of shared resources, we need to keep track of
  // the allocated resources (and not just scalar quantities) on a slave
  // to account for multiple copies of the same shared resources.
  // While the `allocated` info in the `struct slave` gives us just that,
  // we can not simply use that in track/untrackAllocatedResources() since
  // `allocated` is currently updated outside the scope of
  // track/untrackAllocatedResources(), meaning that it may get updated
  // either before or after the tracking calls.
  //
  // TODO(mzhu): Ideally, we want these helpers to instead track the
  // reservations as *allocated* in the sorters even when the
  // reservations have not been allocated yet. This will help to:
  //
  //   (1) Solve the fairness issue when roles with unallocated
  //       reservations may game the allocator (See MESOS-8299).
  //
  //   (2) Simplify the quota enforcement logic -- the allocator
  //       would no longer need to track reservations separately.
  foreachkey (const string& role, quotas) {
    const hashmap<SlaveID, Resources> allocations =
      quotaRoleSorter->allocation(role);

    foreachvalue (const Resources& resources, allocations) {
      // We need to remove the static reservation metadata here via
      // `toUnreserved()`.
      allocatedReservationScalarQuantities[role] +=
        resources.reserved().createStrippedScalarQuantity().toUnreserved();
    }
  }

  // We need to constantly make sure that we are holding back enough unreserved
  // resources that the remaining quota can later be satisfied when needed:
  //
  //   Required unreserved headroom =
  //     sum (unsatisfied quota(r) - unallocated reservations(r))
  //       for each quota role r
  //
  // Given the above, if a role has more reservations than quota,
  // we don't need to hold back any unreserved headroom for it.
  Resources requiredHeadroom;
  foreachpair (const string& role, const Quota& quota, quotas) {
    // NOTE: Revocable resources are excluded in `quotaRoleSorter`.
    // NOTE: Only scalars are considered for quota.
    // NOTE: The following should all be quantities with no meta-data!
    Resources allocated = getQuotaRoleAllocatedResources(role);
    const Resources guarantee = quota.info.guarantee();

    if (allocated.contains(guarantee)) {
      continue; // Quota already satisifed.
    }

    Resources unallocated = guarantee - allocated;

    Resources unallocatedReservations =
      reservationScalarQuantities.get(role).getOrElse(Resources()) -
      allocatedReservationScalarQuantities.get(role).getOrElse(Resources());

    requiredHeadroom += unallocated - unallocatedReservations;
  }

  // We will allocate resources while ensuring that the required
  // unreserved non-revocable headroom is still available. Otherwise,
  // we will not be able to satisfy quota later.
  //
  //   available headroom = unallocated unreserved non-revocable resources
  //
  // We compute this as:
  //
  //   available headroom = total resources -
  //                        allocated resources -
  //                        unallocated reservations -
  //                        unallocated revocable resources

  // NOTE: `totalScalarQuantities` omits dynamic reservation,
  // persistent volume info, and allocation info. We additionally
  // remove the static reservations here via `toUnreserved()`.
  Resources availableHeadroom =
    roleSorter->totalScalarQuantities().toUnreserved();

  // Subtract allocated resources from the total.
  foreachkey (const string& role, roles) {
    // NOTE: `totalScalarQuantities` omits dynamic reservation,
    // persistent volume info, and allocation info. We additionally
    // remove the static reservations here via `toUnreserved()`.
    availableHeadroom -=
      roleSorter->allocationScalarQuantities(role).toUnreserved();
  }

  // Calculate total allocated reservations. Note that we need to ensure
  // we count a reservation for "a" being allocated to "a/b", therefore
  // we cannot simply loop over the reservations' roles.
  Resources totalAllocatedReservationScalarQuantities;
  foreachkey (const string& role, roles) {
    hashmap<SlaveID, Resources> allocations;
    if (quotaRoleSorter->contains(role)) {
      allocations = quotaRoleSorter->allocation(role);
    } else if (roleSorter->contains(role)) {
      allocations = roleSorter->allocation(role);
    } else {
      continue; // This role has no allocation.
    }

    foreachvalue (const Resources& resources, allocations) {
      // NOTE: `totalScalarQuantities` omits dynamic reservation,
      // persistent volume info, and allocation info. We additionally
      // remove the static reservations here via `toUnreserved()`.
      totalAllocatedReservationScalarQuantities +=
        resources.reserved().createStrippedScalarQuantity().toUnreserved();
    }
  }

  // Subtract total unallocated reservations.
  availableHeadroom -=
    Resources::sum(reservationScalarQuantities) -
    totalAllocatedReservationScalarQuantities;

  // Subtract revocable resources.
  foreachvalue (const Slave& slave, slaves) {
    // NOTE: `totalScalarQuantities` omits dynamic reservation,
    // persistent volume info, and allocation info. We additionally
    // remove the static reservations here via `toUnreserved()`.
    availableHeadroom -= slave.getAvailable().revocable()
      .createStrippedScalarQuantity().toUnreserved();
  }

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

      // This is a __quantity__ with no meta-data.
      Resources roleReservationScalarQuantities =
        reservationScalarQuantities.get(role).getOrElse(Resources());

      // This is a __quantity__ with no meta-data.
      Resources roleAllocatedReservationScalarQuantities =
        allocatedReservationScalarQuantities.get(role).getOrElse(Resources());

      // We charge a role against its quota by considering its
      // allocation as well as any unallocated reservations
      // since reservations are bound to the role. In other
      // words, we always consider reservations as consuming
      // quota, regardless of whether they are allocated.
      // The equation used here is:
      //
      //   Consumed Quota = reservations + unreserved allocation
      //                  = reservations + (allocation - allocated reservations)
      //
      // This is a __quantity__ with no meta-data.
      Resources resourcesChargedAgainstQuota =
        roleReservationScalarQuantities +
          (getQuotaRoleAllocatedResources(role) -
               roleAllocatedReservationScalarQuantities);

      // If quota for the role is considered satisfied, then we only
      // further allocate reservations for the role.
      //
      // TODO(alexr): Skipping satisfied roles is pessimistic. Better
      // alternatives are:
      //   * A custom sorter that is aware of quotas and sorts accordingly.
      //   * Removing satisfied roles from the sorter.
      //
      // This is a scalar quantity with no meta-data.
      Resources unsatisfiedQuota = Resources(quota.info.guarantee()) -
        resourcesChargedAgainstQuota;

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
        if (filterGpuResources &&
            !framework.capabilities.gpuResources &&
            slave.getTotal().gpus().getOrElse(0) > 0) {
          continue;
        }

        // If this framework is not region-aware, don't offer it
        // resources on agents in remote regions.
        if (!framework.capabilities.regionAware && isRemoteSlave(slave)) {
          continue;
        }

        // Calculate the currently available resources on the slave, which
        // is the difference in non-shared resources between total and
        // allocated, plus all shared resources on the agent (if applicable).
        // Since shared resources are offerable even when they are in use, we
        // make one copy of the shared resources available regardless of the
        // past allocations.
        Resources available = slave.getAvailable().nonShared();

        // Offer a shared resource only if it has not been offered in
        // this offer cycle to a framework.
        if (framework.capabilities.sharedResources) {
          available += slave.getTotal().shared();
          if (offeredSharedResources.contains(slaveId)) {
            available -= offeredSharedResources[slaveId];
          }
        }

        // We allocate the role's reservations as well as any unreserved
        // resources while ensuring the role stays within its quota limits.
        // This means that we'll "chop" the unreserved resources up to
        // the quota limit if necessary.
        //
        // E.g. A role has no allocations or reservations yet and a 10 cpu
        //      quota limit. We'll chop a 15 cpu agent down to only
        //      allocate 10 cpus to the role to keep it within its limit.
        //
        // In the case that the role needs some of the resources on this
        // agent to make progress towards its quota, or the role is being
        // allocated some reservation(s), we'll *also* allocate all of
        // the resources for which it does not have quota.
        //
        // E.g. The agent has 1 cpu, 1024 mem, 1024 disk, 1 gpu, 5 ports
        //      and the role has quota for 1 cpu, 1024 mem. We'll include
        //      the disk, gpu, and ports in the allocation, despite the
        //      role not having any quota guarantee for them.
        //
        // We have to do this for now because it's not possible to set
        // quota on non-scalar resources, like ports. For scalar resources
        // that this role has no quota for, it can be allocated as long
        // as the quota headroom is not violated.
        //
        // TODO(mzhu): Since we're treating the resources with unset
        // quota as having no guarantee and no limit, these should be
        // also be allocated further in the second allocation "phase"
        // below (above guarantee up to limit).

        // NOTE: Currently, frameworks are allowed to have '*' role.
        // Calling reserved('*') returns an empty Resources object.
        //
        // NOTE: Since we currently only support top-level roles to
        // have quota, there are no ancestor reservations involved here.
        Resources resources = available.reserved(role).nonRevocable();

        // Unreserved resources that are tentatively going to be
        // allocated towards this role's quota. These resources may
        // not get allocated due to framework filters.
        // These are __quantities__ with no meta-data.
        Resources newQuotaAllocationScalarQuantities;

        // We put resource that this role has no quota for in
        // `nonQuotaResources` tentatively.
        Resources nonQuotaResources;

        Resources unreserved = available.nonRevocable().unreserved();

        set<string> quotaResourceNames =
          Resources(quota.info.guarantee()).names();

        // When "chopping" resources, there is more than 1 "chop" that
        // can be done to satisfy the limits. Consider the case with
        // two disks of 1GB, one is PATH and another is MOUNT. And a
        // role has a "disk" quota of 1GB. We could pick either of
        // the disks here, but not both.
        //
        // In order to avoid repeatedly choosing the same "chop" of
        // the resources each time we allocate, we introduce some
        // randomness by shuffling the resources.
        google::protobuf::RepeatedPtrField<Resource>
          resourceVector = unreserved;
        random_shuffle(resourceVector.begin(), resourceVector.end());

        foreach (Resource& resource, resourceVector) {
          if (resource.type() != Value::SCALAR) {
            // We currently do not support quota for non-scalar resources,
            // add it to `nonQuotaResources`. See `nonQuotaResources`
            // regarding how these resources are allocated.
            nonQuotaResources += resource;
            continue;
          }

          if (quotaResourceNames.count(resource.name()) == 0) {
            // Allocating resource that this role has NO quota for,
            // the limit concern here is that it should not break the
            // quota headroom.
            //
            // Allocation Limit = Available Headroom - Required Headroom -
            //                    Tentative Allocation to Role
            Resources upperLimitScalarQuantities =
              availableHeadroom - requiredHeadroom -
              (newQuotaAllocationScalarQuantities +
                nonQuotaResources.createStrippedScalarQuantity());

            Option<Value::Scalar> limitScalar =
              upperLimitScalarQuantities.get<Value::Scalar>(resource.name());

            if (limitScalar.isNone()) {
              continue; // Already have a headroom deficit.
            }

            if (Resources::shrink(&resource, limitScalar.get())) {
              nonQuotaResources += resource;
            }
          } else {
            // Allocating resource that this role has quota for,
            // the limit concern is that it should not exceed this
            // role's unsatisfied quota.
            Resources upperLimitScalarQuantities =
              unsatisfiedQuota - newQuotaAllocationScalarQuantities;

            Option<Value::Scalar> limitScalar =
              upperLimitScalarQuantities.get<Value::Scalar>(resource.name());

            if (limitScalar.isNone()) {
              continue; // Quota limit already met.
            }

            if (Resources::shrink(&resource, limitScalar.get())) {
              resources += resource;
              newQuotaAllocationScalarQuantities +=
                Resources(resource).createStrippedScalarQuantity();
            }
          }
        }

        // We include the non-quota resources (with headroom taken
        // into account) if this role is being allocated some resources
        // already: either some quota resources or a reservation
        // (possibly with quota resources).
        if (!resources.empty()) {
          resources += nonQuotaResources;
        }

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

        // When reservation refinements are present, old frameworks without the
        // RESERVATION_REFINEMENT capability won't be able to understand the
        // new format. While it's possible to translate the refined reservations
        // into the old format by "hiding" the intermediate reservations in the
        // "stack", this leads to ambiguity when processing RESERVE / UNRESERVE
        // operations. This is due to the loss of information when we drop the
        // intermediatereservations. Therefore, for now we simply filter out
        // resources with refined reservations if the framework does not have
        // the capability.
        if (!framework.capabilities.reservationRefinement) {
          resources = resources.filter([](const Resource& resource) {
            return !Resources::hasRefinedReservations(resource);
          });
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

        unsatisfiedQuota -= newQuotaAllocationScalarQuantities;

        // Track quota headroom change.
        requiredHeadroom -= newQuotaAllocationScalarQuantities;
        availableHeadroom -=
          resources.unreserved().createStrippedScalarQuantity();

        // Update the tracking of allocated reservations.
        //
        // Note it is important to do this before updating `slave.allocated`
        // because we rely on `slave.allocated` to check against accounting
        // multiple copies of the same shared resources.
        const Resources newShared = resources.shared()
          .filter([this, &slaveId](const Resources& resource) {
            return !slaves.at(slaveId).getAllocated().contains(resource);
          });

        // We remove the static reservation metadata here via `toUnreserved()`.
        allocatedReservationScalarQuantities[role] +=
          (resources.reserved(role).nonShared() + newShared)
            .createStrippedScalarQuantity().toUnreserved();

        slave.allocate(resources);

        trackAllocatedResources(slaveId, frameworkId, resources);
      }
    }
  }

  // Similar to the first stage, we will allocate resources while ensuring
  // that the required unreserved non-revocable headroom is still available.
  // Otherwise, we will not be able to satisfy quota later. Reservations to
  // non-quota roles and revocable resources will always be included
  // in the offers since these are not part of the headroom (and
  // therefore can't be used to satisfy quota).

  foreach (const SlaveID& slaveId, slaveIds) {
    foreach (const string& role, roleSorter->sort()) {
      // In the second allocation stage, we only allocate
      // for non-quota roles.
      if (quotas.contains(role)) {
        continue;
      }

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
        if (filterGpuResources &&
            !framework.capabilities.gpuResources &&
            slave.getTotal().gpus().getOrElse(0) > 0) {
          continue;
        }

        // If this framework is not region-aware, don't offer it
        // resources on agents in remote regions.
        if (!framework.capabilities.regionAware && isRemoteSlave(slave)) {
          continue;
        }

        // Calculate the currently available resources on the slave, which
        // is the difference in non-shared resources between total and
        // allocated, plus all shared resources on the agent (if applicable).
        // Since shared resources are offerable even when they are in use, we
        // make one copy of the shared resources available regardless of the
        // past allocations.
        Resources available = slave.getAvailable().nonShared();

        // Offer a shared resource only if it has not been offered in
        // this offer cycle to a framework.
        if (framework.capabilities.sharedResources) {
          available += slave.getTotal().shared();
          if (offeredSharedResources.contains(slaveId)) {
            available -= offeredSharedResources[slaveId];
          }
        }

        // The resources we offer are the unreserved resources as well as the
        // reserved resources for this particular role and all its ancestors
        // in the role hierarchy.
        //
        // NOTE: Currently, frameworks are allowed to have '*' role.
        // Calling reserved('*') returns an empty Resources object.
        //
        // TODO(mpark): Offer unreserved resources as revocable beyond quota.
        Resources resources = available.allocatableTo(role);

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

        // When reservation refinements are present, old frameworks without the
        // RESERVATION_REFINEMENT capability won't be able to understand the
        // new format. While it's possible to translate the refined reservations
        // into the old format by "hiding" the intermediate reservations in the
        // "stack", this leads to ambiguity when processing RESERVE / UNRESERVE
        // operations. This is due to the loss of information when we drop the
        // intermediatereservations. Therefore, for now we simply filter out
        // resources with refined reservations if the framework does not have
        // the capability.
        if (!framework.capabilities.reservationRefinement) {
          resources = resources.filter([](const Resource& resource) {
            return !Resources::hasRefinedReservations(resource);
          });
        }

        // If allocating these resources would reduce the headroom
        // below what is required, we will hold them back.
        const Resources headroomToAllocate = resources
          .scalars().unreserved().nonRevocable();

        bool sufficientHeadroom =
          (availableHeadroom -
            headroomToAllocate.createStrippedScalarQuantity())
            .contains(requiredHeadroom);

        if (!sufficientHeadroom) {
          resources -= headroomToAllocate;
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

        if (sufficientHeadroom) {
          availableHeadroom -=
            headroomToAllocate.createStrippedScalarQuantity();
        }

        slave.allocate(resources);

        trackAllocatedResources(slaveId, frameworkId, resources);
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

  // TODO(mpark): Consider moving these filter logic out and into the master,
  // since they are not specific to the hierarchical allocator but rather are
  // global allocation constraints.

  // Prevent offers from non-MULTI_ROLE agents to be allocated
  // to MULTI_ROLE frameworks.
  if (framework.capabilities.multiRole &&
      !slave.capabilities.multiRole) {
    LOG(WARNING) << "Implicitly filtering agent " << slaveId
                 << " from framework " << frameworkId
                 << " because the framework is MULTI_ROLE capable"
                 << " but the agent is not";

    return true;
  }

  // Prevent offers from non-HIERARCHICAL_ROLE agents to be allocated
  // to hierarchical roles.
  if (!slave.capabilities.hierarchicalRole && strings::contains(role, "/")) {
    LOG(WARNING) << "Implicitly filtering agent " << slaveId << " from role "
                 << role << " because the role is hierarchical but the agent"
                 << " is not HIERARCHICAL_ROLE capable";

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


bool HierarchicalAllocatorProcess::allocatable(const Resources& resources)
{
  if (minAllocatableResources.isNone() ||
      CHECK_NOTNONE(minAllocatableResources).empty()) {
    return true;
  }

  // We remove the static reservation metadata here via `toUnreserved()`.
  Resources quantity = resources.createStrippedScalarQuantity().toUnreserved();
  foreach (
      const Resources& minResources, CHECK_NOTNONE(minAllocatableResources)) {
    if (quantity.contains(minResources)) {
      return true;
    }
  }

  return false;
}


double HierarchicalAllocatorProcess::_resources_offered_or_allocated(
    const string& resource)
{
  double offered_or_allocated = 0;

  foreachvalue (const Slave& slave, slaves) {
    Option<Value::Scalar> value =
      slave.getAllocated().get<Value::Scalar>(resource);

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
  if (!roleSorter->contains(role)) {
    // This can occur when execution of this callback races with removal of the
    // metric for a role which does not have any associated frameworks.
    return 0.;
  }

  Option<Value::Scalar> used =
    roleSorter->allocationScalarQuantities(role)
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


void HierarchicalAllocatorProcess::trackReservations(
    const hashmap<std::string, Resources>& reservations)
{
  foreachpair (const string& role,
               const Resources& resources, reservations) {
    // We remove the static reservation metadata here via `toUnreserved()`.
    const Resources scalarQuantitesToTrack =
        resources.createStrippedScalarQuantity().toUnreserved();

    reservationScalarQuantities[role] += scalarQuantitesToTrack;
  }
}


void HierarchicalAllocatorProcess::untrackReservations(
    const hashmap<std::string, Resources>& reservations)
{
  foreachpair (const string& role,
               const Resources& resources, reservations) {
    CHECK(reservationScalarQuantities.contains(role));
    Resources& currentReservationQuantity =
        reservationScalarQuantities.at(role);

    // We remove the static reservation metadata here via `toUnreserved()`.
    const Resources scalarQuantitesToUntrack =
        resources.createStrippedScalarQuantity().toUnreserved();
    CHECK(currentReservationQuantity.contains(scalarQuantitesToUntrack));
    currentReservationQuantity -= scalarQuantitesToUntrack;

    if (currentReservationQuantity.empty()) {
      reservationScalarQuantities.erase(role);
    }
  }
}


bool HierarchicalAllocatorProcess::updateSlaveTotal(
    const SlaveID& slaveId,
    const Resources& total)
{
  CHECK(slaves.contains(slaveId));

  Slave& slave = slaves.at(slaveId);

  const Resources oldTotal = slave.getTotal();

  if (oldTotal == total) {
    return false;
  }

  slave.updateTotal(total);

  hashmap<std::string, Resources> oldReservations = oldTotal.reservations();
  hashmap<std::string, Resources> newReservations = total.reservations();

  if (oldReservations != newReservations) {
    untrackReservations(oldReservations);
    trackReservations(newReservations);
  }

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

  return true;
}


bool HierarchicalAllocatorProcess::isRemoteSlave(const Slave& slave) const
{
  // If the slave does not have a configured domain, assume it is not remote.
  if (slave.domain.isNone()) {
    return false;
  }

  // The current version of the Mesos agent refuses to startup if a
  // domain is specified without also including a fault domain. That
  // might change in the future, if more types of domains are added.
  // For forward compatibility, we treat agents with a configured
  // domain but no fault domain as having no configured domain.
  if (!slave.domain->has_fault_domain()) {
    return false;
  }

  // If the slave has a configured domain (and it has been allowed to
  // register with the master), the master must also have a configured
  // domain.
  CHECK(domain.isSome());

  // The master will not startup if configured with a domain but no
  // fault domain.
  CHECK(domain->has_fault_domain());

  const DomainInfo::FaultDomain::RegionInfo& masterRegion =
    domain->fault_domain().region();
  const DomainInfo::FaultDomain::RegionInfo& slaveRegion =
    slave.domain->fault_domain().region();

  return masterRegion != slaveRegion;
}


void HierarchicalAllocatorProcess::trackAllocatedResources(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Resources& allocated)
{
  CHECK(slaves.contains(slaveId));
  CHECK(frameworks.contains(frameworkId));

  // TODO(bmahler): Calling allocations() is expensive since it has
  // to construct a map. Avoid this.
  foreachpair (const string& role,
               const Resources& allocation,
               allocated.allocations()) {
    // The framework has resources allocated to this role but it may
    // or may not be subscribed to the role. Either way, we need to
    // track the framework under the role.
    if (!isFrameworkTrackedUnderRole(frameworkId, role)) {
      trackFrameworkUnderRole(frameworkId, role);
    }

    CHECK(roleSorter->contains(role));
    CHECK(frameworkSorters.contains(role));
    CHECK(frameworkSorters.at(role)->contains(frameworkId.value()));

    roleSorter->allocated(role, slaveId, allocation);
    frameworkSorters.at(role)->add(slaveId, allocation);
    frameworkSorters.at(role)->allocated(
        frameworkId.value(), slaveId, allocation);

    if (quotas.contains(role)) {
      // See comment at `quotaRoleSorter` declaration regarding non-revocable.
      quotaRoleSorter->allocated(role, slaveId, allocation.nonRevocable());
    }
  }
}


void HierarchicalAllocatorProcess::untrackAllocatedResources(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Resources& allocated)
{
  // TODO(mzhu): Add a `CHECK(slaves.contains(slaveId));`
  // here once MESOS-621 is resolved. Ideally, `removeSlave()`
  // should unallocate resources in the framework sorters.
  // But currently, a slave is removed first via `removeSlave()`
  // and later a call to `recoverResources()` occurs to recover
  // the framework's resources.
  CHECK(frameworks.contains(frameworkId));

  // TODO(bmahler): Calling allocations() is expensive since it has
  // to construct a map. Avoid this.
  foreachpair (const string& role,
               const Resources& allocation,
               allocated.allocations()) {
    CHECK(roleSorter->contains(role));
    CHECK(frameworkSorters.contains(role));
    CHECK(frameworkSorters.at(role)->contains(frameworkId.value()));

    frameworkSorters.at(role)->unallocated(
        frameworkId.value(), slaveId, allocation);
    frameworkSorters.at(role)->remove(slaveId, allocation);

    roleSorter->unallocated(role, slaveId, allocation);

    if (quotas.contains(role)) {
      // See comment at `quotaRoleSorter` declaration regarding non-revocable.
      quotaRoleSorter->unallocated(role, slaveId, allocation.nonRevocable());
    }
  }
}

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
