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

#include "master/allocator/mesos/hierarchical.hpp"

#include <algorithm>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/event.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/hashset.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>

using std::string;
using std::vector;

using mesos::master::InverseOfferStatus;
using mesos::master::RoleInfo;

using mesos::quota::QuotaInfo;

using process::Failure;
using process::Future;
using process::Timeout;

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

  virtual bool filter(const Resources& resources) = 0;
};


class RefusedOfferFilter : public OfferFilter
{
public:
  RefusedOfferFilter(const Resources& _resources) : resources(_resources) {}

  virtual bool filter(const Resources& _resources)
  {
    // TODO(jieyu): Consider separating the superset check for regular
    // and revocable resources. For example, frameworks might want
    // more revocable resources only or non-revocable resources only,
    // but currently the filter only expires if there is more of both
    // revocable and non-revocable resources.
    return resources.contains(_resources); // Refused resources are superset.
  }

  const Resources resources;
};


// Used to represent "filters" for inverse offers.
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

  virtual bool filter() = 0;
};


// NOTE: See comment above `InverseOfferFilter` regarding capturing
// `unavailableResources` if this allocator starts sending fine-grained inverse
// offers.
class RefusedInverseOfferFilter : public InverseOfferFilter
{
public:
  RefusedInverseOfferFilter(const Timeout& _timeout)
    : timeout(_timeout) {}

  virtual bool filter()
  {
    // See comment above why we currently don't do more fine-grained filtering.
    return timeout.remaining() > Seconds(0);
  }

  const Timeout timeout;
};


void HierarchicalAllocatorProcess::initialize(
    const Duration& _allocationInterval,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, Resources>&)>& _offerCallback,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, UnavailableResources>&)>&
      _inverseOfferCallback,
    const hashmap<string, RoleInfo>& _roles)
{
  allocationInterval = _allocationInterval;
  offerCallback = _offerCallback;
  inverseOfferCallback = _inverseOfferCallback;
  roles = _roles;
  initialized = true;

  roleSorter = roleSorterFactory();
  foreachpair (const string& name, const RoleInfo& roleInfo, roles) {
    roleSorter->add(name, roleInfo.weight());
    frameworkSorters[name] = frameworkSorterFactory();
  }

  if (roleSorter->count() == 0) {
    LOG(ERROR) << "No roles specified, cannot allocate resources!";
  }

  VLOG(1) << "Initialized hierarchical allocator process";

  delay(allocationInterval, self(), &Self::batch);
}


void HierarchicalAllocatorProcess::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const hashmap<SlaveID, Resources>& used)
{
  const string& role = frameworkInfo.role();

  CHECK(initialized);
  CHECK(roles.contains(role));
  CHECK(!frameworkSorters[role]->contains(frameworkId.value()));

  frameworkSorters[role]->add(frameworkId.value());

  // TODO(bmahler): Validate that the reserved resources have the
  // framework's role.

  // Update the allocation to this framework.
  foreachpair (const SlaveID& slaveId, const Resources& allocated, used) {
    roleSorter->allocated(role, slaveId, allocated.unreserved());
    frameworkSorters[role]->add(slaveId, allocated);
    frameworkSorters[role]->allocated(frameworkId.value(), slaveId, allocated);
  }

  frameworks[frameworkId] = Framework();
  frameworks[frameworkId].role = frameworkInfo.role();
  frameworks[frameworkId].checkpoint = frameworkInfo.checkpoint();

  // Check if the framework desires revocable resources.
  frameworks[frameworkId].revocable = false;
  foreach (const FrameworkInfo::Capability& capability,
           frameworkInfo.capabilities()) {
    if (capability.type() == FrameworkInfo::Capability::REVOCABLE_RESOURCES) {
      frameworks[frameworkId].revocable = true;
    }
  }

  frameworks[frameworkId].suppressed = false;

  LOG(INFO) << "Added framework " << frameworkId;

  allocate();
}


void HierarchicalAllocatorProcess::removeFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  const string& role = frameworks[frameworkId].role;

  // Might not be in 'frameworkSorters[role]' because it was previously
  // deactivated and never re-added.
  if (frameworkSorters[role]->contains(frameworkId.value())) {
    hashmap<SlaveID, Resources> allocation =
      frameworkSorters[role]->allocation(frameworkId.value());

    foreachpair (
        const SlaveID& slaveId, const Resources& allocated, allocation) {
      roleSorter->unallocated(role, slaveId, allocated.unreserved());
      frameworkSorters[role]->remove(slaveId, allocated);
    }

    frameworkSorters[role]->remove(frameworkId.value());
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

  const string& role = frameworks[frameworkId].role;

  frameworkSorters[role]->activate(frameworkId.value());

  LOG(INFO) << "Activated framework " << frameworkId;

  allocate();
}


void HierarchicalAllocatorProcess::deactivateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  const string& role = frameworks[frameworkId].role;

  frameworkSorters[role]->deactivate(frameworkId.value());

  // Note that the Sorter *does not* remove the resources allocated
  // to this framework. For now, this is important because if the
  // framework fails over and is activated, we still want a record
  // of the resources that it is using. We might be able to collapse
  // the added/removed and activated/deactivated in the future.

  // Do not delete the filters contained in this
  // framework's `offerFilters` hashset yet, see comments in
  // HierarchicalAllocatorProcess::reviveOffers and
  // HierarchicalAllocatorProcess::expire.
  frameworks[frameworkId].offerFilters.clear();
  frameworks[frameworkId].inverseOfferFilters.clear();

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


void HierarchicalAllocatorProcess::updateFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  // TODO(jmlvanre): Once we allow frameworks to re-register with a
  // new 'role' or 'checkpoint' flag, we need to update our internal
  // 'frameworks' structure. See MESOS-703 for progress on allowing
  // these fields to be updated.
  CHECK_EQ(frameworks[frameworkId].role, frameworkInfo.role());
  CHECK_EQ(frameworks[frameworkId].checkpoint, frameworkInfo.checkpoint());

  frameworks[frameworkId].revocable = false;

  foreach (const FrameworkInfo::Capability& capability,
           frameworkInfo.capabilities()) {
    if (capability.type() == FrameworkInfo::Capability::REVOCABLE_RESOURCES) {
      frameworks[frameworkId].revocable = true;
    }
  }
}


void HierarchicalAllocatorProcess::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const Option<Unavailability>& unavailability,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);
  CHECK(!slaves.contains(slaveId));

  roleSorter->add(slaveId, total.unreserved());

  foreachpair (const FrameworkID& frameworkId,
               const Resources& allocated,
               used) {
    if (frameworks.contains(frameworkId)) {
      const string& role = frameworks[frameworkId].role;

      // TODO(bmahler): Validate that the reserved resources have the
      // framework's role.

      roleSorter->allocated(role, slaveId, allocated.unreserved());
      frameworkSorters[role]->add(slaveId, allocated);
      frameworkSorters[role]->allocated(
          frameworkId.value(), slaveId, allocated);
    }
  }

  slaves[slaveId] = Slave();
  slaves[slaveId].total = total;
  slaves[slaveId].allocated = Resources::sum(used);
  slaves[slaveId].activated = true;
  slaves[slaveId].checkpoint = slaveInfo.checkpoint();
  slaves[slaveId].hostname = slaveInfo.hostname();

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.
  if (unavailability.isSome()) {
    slaves[slaveId].maintenance =
      typename Slave::Maintenance(unavailability.get());
  }

  LOG(INFO) << "Added slave " << slaveId << " (" << slaves[slaveId].hostname
            << ") with " << slaves[slaveId].total
            << " (allocated: " << slaves[slaveId].allocated << ")";

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

  roleSorter->remove(slaveId, slaves[slaveId].total.unreserved());

  slaves.erase(slaveId);

  // Note that we DO NOT actually delete any filters associated with
  // this slave, that will occur when the delayed
  // HierarchicalAllocatorProcess::expire gets invoked (or the framework
  // that applied the filters gets removed).

  LOG(INFO) << "Removed slave " << slaveId;
}


void HierarchicalAllocatorProcess::updateSlave(
    const SlaveID& slaveId,
    const Resources& oversubscribed)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  // Check that all the oversubscribed resources are revocable.
  CHECK_EQ(oversubscribed, oversubscribed.revocable());

  // Update the total resources.

  // First remove the old oversubscribed resources from the total.
  slaves[slaveId].total -= slaves[slaveId].total.revocable();

  // Now add the new estimate of oversubscribed resources.
  slaves[slaveId].total += oversubscribed;

  // Now, update the total resources in the role sorter.
  roleSorter->update(
      slaveId,
      slaves[slaveId].total.unreserved());

  LOG(INFO) << "Slave " << slaveId << " (" << slaves[slaveId].hostname << ")"
            << " updated with oversubscribed resources " << oversubscribed
            << " (total: " << slaves[slaveId].total
            << ", allocated: " << slaves[slaveId].allocated << ")";

  allocate(slaveId);
}


void HierarchicalAllocatorProcess::activateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].activated = true;

  LOG(INFO)<< "Slave " << slaveId << " reactivated";
}


void HierarchicalAllocatorProcess::deactivateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].activated = false;

  LOG(INFO) << "Slave " << slaveId << " deactivated";
}


void HierarchicalAllocatorProcess::updateWhitelist(
    const Option<hashset<string>>& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated slave whitelist: " << stringify(whitelist.get());

    if (whitelist.get().empty()) {
      LOG(WARNING) << "Whitelist is empty, no offers will be made!";
    }
  } else {
    LOG(INFO) << "Advertising offers for all slaves";
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
    const vector<Offer::Operation>& operations)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));
  CHECK(frameworks.contains(frameworkId));

  const string& role = frameworks[frameworkId].role;

  // Here we apply offer operations to the allocated resources, which
  // in turns leads to an update of the total. The available resources
  // remain unchanged.

  // Update the allocated resources.
  Sorter* frameworkSorter = frameworkSorters[role];

  Resources frameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  Try<Resources> updatedFrameworkAllocation =
    frameworkAllocation.apply(operations);

  CHECK_SOME(updatedFrameworkAllocation);

  frameworkSorter->update(
      frameworkId.value(),
      slaveId,
      frameworkAllocation,
      updatedFrameworkAllocation.get());

  roleSorter->update(
      role,
      slaveId,
      frameworkAllocation.unreserved(),
      updatedFrameworkAllocation.get().unreserved());

  Try<Resources> updatedSlaveAllocation =
    slaves[slaveId].allocated.apply(operations);

  CHECK_SOME(updatedSlaveAllocation);

  slaves[slaveId].allocated = updatedSlaveAllocation.get();

  // Update the total resources.
  Try<Resources> updatedTotal = slaves[slaveId].total.apply(operations);
  CHECK_SOME(updatedTotal);

  slaves[slaveId].total = updatedTotal.get();

  LOG(INFO) << "Updated allocation of framework " << frameworkId
            << " on slave " << slaveId
            << " from " << frameworkAllocation
            << " to " << updatedFrameworkAllocation.get();
}


Future<Nothing> HierarchicalAllocatorProcess::updateAvailable(
    const SlaveID& slaveId,
    const vector<Offer::Operation>& operations)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  Resources available = slaves[slaveId].total - slaves[slaveId].allocated;

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
  Try<Resources> updatedAvailable = available.apply(operations);
  if (updatedAvailable.isError()) {
    return Failure(updatedAvailable.error());
  }

  // Update the total resources.
  Try<Resources> updatedTotal = slaves[slaveId].total.apply(operations);
  CHECK_SOME(updatedTotal);

  slaves[slaveId].total = updatedTotal.get();

  // Now, update the total resources in the role sorter.
  roleSorter->update(slaveId, slaves[slaveId].total.unreserved());

  return Nothing();
}


void HierarchicalAllocatorProcess::updateUnavailability(
    const SlaveID& slaveId,
    const Option<Unavailability>& unavailability)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

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
  slaves[slaveId].maintenance = None();

  // If we have a new unavailability.
  if (unavailability.isSome()) {
    slaves[slaveId].maintenance =
      typename Slave::Maintenance(unavailability.get());
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
  CHECK(slaves[slaveId].maintenance.isSome());

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.

  // We use a reference by alias because we intend to modify the
  // `maintenance` and to improve readability.
  typename Slave::Maintenance& maintenance = slaves[slaveId].maintenance.get();

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
            << " filtered inverse offers from slave " << slaveId
            << " for " << seconds.get();

    // Create a new inverse offer filter and delay its expiration.
    InverseOfferFilter* inverseOfferFilter =
      new RefusedInverseOfferFilter(Timeout::in(seconds.get()));

    frameworks[frameworkId]
      .inverseOfferFilters[slaveId].insert(inverseOfferFilter);

    // We need to disambiguate the function call to pick the correct
    // expire() overload.
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

  // Updated resources allocated to framework (if framework still
  // exists, which it might not in the event that we dispatched
  // Master::offer before we received
  // MesosAllocatorProcess::removeFramework or
  // MesosAllocatorProcess::deactivateFramework, in which case we will
  // have already recovered all of its resources).
  if (frameworks.contains(frameworkId)) {
    const string& role = frameworks[frameworkId].role;

    CHECK(frameworkSorters.contains(role));

    if (frameworkSorters[role]->contains(frameworkId.value())) {
      frameworkSorters[role]->unallocated(
          frameworkId.value(), slaveId, resources);
      frameworkSorters[role]->remove(slaveId, resources);
      roleSorter->unallocated(role, slaveId, resources.unreserved());
    }
  }

  // Update resources allocated on slave (if slave still exists,
  // which it might not in the event that we dispatched Master::offer
  // before we received Allocator::removeSlave).
  if (slaves.contains(slaveId)) {
    CHECK(slaves[slaveId].allocated.contains(resources));

    slaves[slaveId].allocated -= resources;

    LOG(INFO) << "Recovered " << resources
              << " (total: " << slaves[slaveId].total
              << ", allocated: " << slaves[slaveId].allocated
              << ") on slave " << slaveId
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
  Try<Duration> seconds = Duration::create(filters.get().refuse_seconds());

  if (seconds.isError()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused resources filter because the input value "
                 << "is invalid: " << seconds.error();

    seconds = Duration::create(Filters().refuse_seconds());
  } else if (seconds.get() < Duration::zero()) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                 << "the refused resources filter because the input value "
                 << "is negative";

    seconds = Duration::create(Filters().refuse_seconds());
  }

  CHECK_SOME(seconds);

  if (seconds.get() != Duration::zero()) {
    VLOG(1) << "Framework " << frameworkId
            << " filtered slave " << slaveId
            << " for " << seconds.get();

    // Create a new filter.
    OfferFilter* offerFilter = new RefusedOfferFilter(resources);
    frameworks[frameworkId].offerFilters[slaveId].insert(offerFilter);

    // We need to disambiguate the function call to pick the correct
    // expire() overload.
    void (Self::*expireOffer)(
              const FrameworkID&,
              const SlaveID&,
              OfferFilter*) = &Self::expire;

    // Expire the filter after both an `allocationInterval` and the
    // `timeout` have elapsed. This ensures that the filter does not
    // expire before we perform the next allocation for this agent,
    // see MESOS-4302 for more information.
    //
    // TODO(alexr): If we allocated upon resource recovery
    // (MESOS-3078), we would not need to increase the timeout here.
    Duration timeout = std::max(allocationInterval, seconds.get());

    delay(timeout,
          self(),
          expireOffer,
          frameworkId,
          slaveId,
          offerFilter);
  }
}


void HierarchicalAllocatorProcess::suppressOffers(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  frameworks[frameworkId].suppressed = true;

  LOG(INFO) << "Suppressed offers for framework " << frameworkId;
}


void HierarchicalAllocatorProcess::reviveOffers(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  frameworks[frameworkId].offerFilters.clear();
  frameworks[frameworkId].inverseOfferFilters.clear();
  frameworks[frameworkId].suppressed = false;

  // We delete each actual `OfferFilter` when
  // `HierarchicalAllocatorProcess::expire` gets invoked. If we delete the
  // `OfferFilter` here it's possible that the same `OfferFilter` (i.e., same
  // address) could get reused and `HierarchicalAllocatorProcess::expire`
  // would expire that filter too soon. Note that this only works
  // right now because ALL Filter types "expire".

  LOG(INFO) << "Removed offer filters for framework " << frameworkId;

  allocate();
}


void HierarchicalAllocatorProcess::setQuota(
    const string& role,
    const QuotaInfo& quota)
{
  CHECK(initialized);

  LOG(INFO) << "Received quota set request for role " << role;
}


void HierarchicalAllocatorProcess::removeQuota(
    const string& role)
{
  CHECK(initialized);

  LOG(INFO) << "Received quota remove request for role " << role;
}


void HierarchicalAllocatorProcess::batch()
{
  allocate();
  delay(allocationInterval, self(), &Self::batch);
}


void HierarchicalAllocatorProcess::allocate()
{
  Stopwatch stopwatch;
  stopwatch.start();

  allocate(slaves.keys());

  VLOG(1) << "Performed allocation for " << slaves.size() << " slaves in "
            << stopwatch.elapsed();
}


void HierarchicalAllocatorProcess::allocate(
    const SlaveID& slaveId)
{
  Stopwatch stopwatch;
  stopwatch.start();

  // TODO(bmahler): Add initializer list constructor for hashset.
  hashset<SlaveID> slaves;
  slaves.insert(slaveId);
  allocate(slaves);

  VLOG(1) << "Performed allocation for slave " << slaveId << " in "
          << stopwatch.elapsed();
}


void HierarchicalAllocatorProcess::allocate(
    const hashset<SlaveID>& slaveIds_)
{
  if (roleSorter->count() == 0) {
    LOG(ERROR) << "No roles specified, cannot allocate resources!";
    return;
  }

  // Compute the offerable resources, per framework:
  //   (1) For reserved resources on the slave, allocate these to a
  //       framework having the corresponding role.
  //   (2) For unreserved resources on the slave, allocate these
  //       to a framework of any role.
  hashmap<FrameworkID, hashmap<SlaveID, Resources>> offerable;

  // Randomize the order in which slaves' resources are allocated.
  // TODO(vinod): Implement a smarter sorting algorithm.
  vector<SlaveID> slaveIds(slaveIds_.begin(), slaveIds_.end());
  std::random_shuffle(slaveIds.begin(), slaveIds.end());

  foreach (const SlaveID& slaveId, slaveIds) {
    // Don't send offers for non-whitelisted and deactivated slaves.
    if (!isWhitelisted(slaveId) || !slaves[slaveId].activated) {
      continue;
    }

    foreach (const string& role, roleSorter->sort()) {
      foreach (const string& frameworkId_,
               frameworkSorters[role]->sort()) {
        FrameworkID frameworkId;
        frameworkId.set_value(frameworkId_);

        // If the framework has suppressed offers, ignore.
        if (frameworks[frameworkId].suppressed) {
          continue;
        }

        // Calculate the currently available resources on the slave.
        Resources available = slaves[slaveId].total - slaves[slaveId].allocated;

        // NOTE: Currently, frameworks are allowed to have '*' role.
        // Calling reserved('*') returns an empty Resources object.
        Resources resources = available.unreserved() + available.reserved(role);

        // Remove revocable resources if the framework has not opted
        // for them.
        if (!frameworks[frameworkId].revocable) {
          resources -= resources.revocable();
        }

        // If the resources are not allocatable, ignore.
        if (!allocatable(resources)) {
          continue;
        }

        // If the framework filters these resources, ignore.
        if (isFiltered(frameworkId, slaveId, resources)) {
          continue;
        }

        VLOG(2) << "Allocating " << resources << " on slave " << slaveId
                << " to framework " << frameworkId;

        // Note that we perform "coarse-grained" allocation,
        // meaning that we always allocate the entire remaining
        // slave resources to a single framework.
        offerable[frameworkId][slaveId] = resources;
        slaves[slaveId].allocated += resources;

        // Reserved resources are only accounted for in the framework
        // sorter, since the reserved resources are not shared across
        // roles.
        frameworkSorters[role]->add(slaveId, resources);
        frameworkSorters[role]->allocated(frameworkId_, slaveId, resources);
        roleSorter->allocated(role, slaveId, resources.unreserved());
      }
    }
  }

  if (offerable.empty()) {
    VLOG(1) << "No resources available to allocate!";
  } else {
    // Now offer the resources to each framework.
    foreachkey (const FrameworkID& frameworkId, offerable) {
      offerCallback(frameworkId, offerable[frameworkId]);
    }
  }

  // NOTE: For now, we implement maintenance inverse offers within the
  // allocator. We leverage the existing timer/cycle of offers to also do any
  // "deallocation" (inverse offers) necessary to satisfy maintenance needs.
  deallocate(slaveIds_);
}


void HierarchicalAllocatorProcess::deallocate(
    const hashset<SlaveID>& slaveIds_)
{
  if (frameworkSorters.empty()) {
    LOG(ERROR) << "No frameworks specified, cannot send inverse offers!";
    return;
  }

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

  foreachvalue (Sorter* frameworkSorter, frameworkSorters) {
    foreach (const SlaveID& slaveId, slaveIds_) {
      CHECK(slaves.contains(slaveId));

      if (slaves[slaveId].maintenance.isSome()) {
        // We use a reference by alias because we intend to modify the
        // `maintenance` and to improve readability.
        typename Slave::Maintenance& maintenance =
          slaves[slaveId].maintenance.get();

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

              // Mark this framework as having an offer oustanding for the
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


void HierarchicalAllocatorProcess::expire(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    OfferFilter* offerFilter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in
  // HierarchicalAllocatorProcess::reviveOffers) but not yet deleted (to
  // keep the address from getting reused possibly causing premature
  // expiration).
  if (frameworks.contains(frameworkId) &&
      frameworks[frameworkId].offerFilters.contains(slaveId) &&
      frameworks[frameworkId].offerFilters[slaveId].contains(offerFilter)) {
    frameworks[frameworkId].offerFilters[slaveId].erase(offerFilter);
    if (frameworks[frameworkId].offerFilters[slaveId].empty()) {
      frameworks[frameworkId].offerFilters.erase(slaveId);
    }
  }

  delete offerFilter;
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
  if (frameworks.contains(frameworkId) &&
      frameworks[frameworkId].inverseOfferFilters.contains(slaveId) &&
      frameworks[frameworkId].inverseOfferFilters[slaveId]
        .contains(inverseOfferFilter)) {
    frameworks[frameworkId].inverseOfferFilters[slaveId]
      .erase(inverseOfferFilter);

    if(frameworks[frameworkId].inverseOfferFilters[slaveId].empty()) {
      frameworks[frameworkId].inverseOfferFilters.erase(slaveId);
    }
  }

  delete inverseOfferFilter;
}


bool HierarchicalAllocatorProcess::isWhitelisted(
    const SlaveID& slaveId)
{
  CHECK(slaves.contains(slaveId));

  return whitelist.isNone() ||
         whitelist.get().contains(slaves[slaveId].hostname);
}


bool HierarchicalAllocatorProcess::isFiltered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(frameworks.contains(frameworkId));
  CHECK(slaves.contains(slaveId));

  // Do not offer a non-checkpointing slave's resources to a checkpointing
  // framework. This is a short term fix until the following is resolved:
  // https://issues.apache.org/jira/browse/MESOS-444.
  if (frameworks[frameworkId].checkpoint && !slaves[slaveId].checkpoint) {
    VLOG(1) << "Filtered offer with " << resources
            << " on non-checkpointing slave " << slaveId
            << " for checkpointing framework " << frameworkId;

    return true;
  }

  if (frameworks[frameworkId].offerFilters.contains(slaveId)) {
    foreach (
      OfferFilter* offerFilter, frameworks[frameworkId].offerFilters[slaveId]) {
      if (offerFilter->filter(resources)) {
        VLOG(1) << "Filtered offer with " << resources
                << " on slave " << slaveId
                << " for framework " << frameworkId;

        return true;
      }
    }
  }

  return false;
}


bool HierarchicalAllocatorProcess::isFiltered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId)
{
  CHECK(frameworks.contains(frameworkId));
  CHECK(slaves.contains(slaveId));

  if (frameworks[frameworkId].inverseOfferFilters.contains(slaveId)) {
    foreach (
        InverseOfferFilter* inverseOfferFilter,
        frameworks[frameworkId].inverseOfferFilters[slaveId]) {
      if (inverseOfferFilter->filter()) {
        VLOG(1) << "Filtered unavailability on slave " << slaveId
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

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
