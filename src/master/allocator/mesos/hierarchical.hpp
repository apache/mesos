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

#ifndef __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__
#define __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__

#include <algorithm>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/event.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>

#include "master/allocator/mesos/allocator.hpp"
#include "master/allocator/sorter/drf/sorter.hpp"

#include "master/constants.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// Forward declarations.
class OfferFilter;
class InverseOfferFilter;

// We forward declare the hierarchical allocator process so that we
// can typedef an instantiation of it with DRF sorters.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess;

typedef HierarchicalAllocatorProcess<DRFSorter, DRFSorter>
HierarchicalDRFAllocatorProcess;

typedef MesosAllocator<HierarchicalDRFAllocatorProcess>
HierarchicalDRFAllocator;


// Implements the basic allocator algorithm - first pick a role by
// some criteria, then pick one of their frameworks to allocate to.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess : public MesosAllocatorProcess
{
public:
  HierarchicalAllocatorProcess()
    : ProcessBase(process::ID::generate("hierarchical-allocator")),
      initialized(false),
      metrics(*this),
      roleSorter(NULL) {}

  virtual ~HierarchicalAllocatorProcess() {}

  process::PID<HierarchicalAllocatorProcess> self() const
  {
    return process::PID<Self>(this);
  }

  void initialize(
      const Duration& allocationInterval,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>& offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback,
      const hashmap<std::string, mesos::master::RoleInfo>& roles);

  void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used);

  void removeFramework(
      const FrameworkID& frameworkId);

  void activateFramework(
      const FrameworkID& frameworkId);

  void deactivateFramework(
      const FrameworkID& frameworkId);

  void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo);

  void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used);

  void removeSlave(
      const SlaveID& slaveId);

  void updateSlave(
      const SlaveID& slave,
      const Resources& oversubscribed);

  void deactivateSlave(
      const SlaveID& slaveId);

  void activateSlave(
      const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string>>& whitelist);

  void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations);

  process::Future<Nothing> updateAvailable(
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations);

  void updateUnavailability(
      const SlaveID& slaveId,
      const Option<Unavailability>& unavailability);

  void updateInverseOffer(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const Option<UnavailableResources>& unavailableResources,
      const Option<mesos::master::InverseOfferStatus>& status,
      const Option<Filters>& filters);

  process::Future<
      hashmap<SlaveID, hashmap<FrameworkID, mesos::master::InverseOfferStatus>>>
    getInverseOfferStatuses();

  void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void suppressOffers(
      const FrameworkID& frameworkId);

  void reviveOffers(
      const FrameworkID& frameworkId);

protected:
  // Useful typedefs for dispatch/delay/defer to self()/this.
  typedef HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter> Self;
  typedef HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter> This;

  // Callback for doing batch allocations.
  void batch();

  // Allocate any allocatable resources.
  void allocate();

  // Allocate resources just from the specified slave.
  void allocate(const SlaveID& slaveId);

  // Allocate resources from the specified slaves.
  void allocate(const hashset<SlaveID>& slaveIds);

  // Send inverse offers from the specified slaves.
  void deallocate(const hashset<SlaveID>& slaveIds);

  // Remove an offer filter for the specified framework.
  void expire(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      OfferFilter* offerFilter);

  // Remove an inverse offer filter for the specified framework.
  void expire(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      InverseOfferFilter* inverseOfferFilter);

  // Checks whether the slave is whitelisted.
  bool isWhitelisted(const SlaveID& slaveId);

  // Returns true if there is a resource offer filter for this framework
  // on this slave.
  bool isFiltered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources);

  // Returns true if there is an inverse offer filter for this framework
  // on this slave.
  bool isFiltered(
      const FrameworkID& frameworkID,
      const SlaveID& slaveID);

  bool allocatable(const Resources& resources);

  bool initialized;

  Duration allocationInterval;

  lambda::function<
      void(const FrameworkID&,
           const hashmap<SlaveID, Resources>&)> offerCallback;

  lambda::function<
      void(const FrameworkID&,
           const hashmap<SlaveID, UnavailableResources>&)> inverseOfferCallback;

  struct Metrics
  {
    explicit Metrics(const Self& process)
      : event_queue_dispatches(
            "allocator/event_queue_dispatches",
            process::defer(process.self(), &Self::_event_queue_dispatches))
    {
      process::metrics::add(event_queue_dispatches);
    }

    ~Metrics()
    {
      process::metrics::remove(event_queue_dispatches);
    }

    process::metrics::Gauge event_queue_dispatches;
  } metrics;

  struct Framework
  {
    std::string role;
    bool checkpoint;  // Whether the framework desires checkpointing.

    // Whether the framework suppresses offers.
    bool suppressed;

    // Whether the framework desires revocable resources.
    bool revocable;

    // Active offer and inverse offer filters for the framework.
    hashmap<SlaveID, hashset<OfferFilter*>> offerFilters;
    hashmap<SlaveID, hashset<InverseOfferFilter*>> inverseOfferFilters;
  };

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<process::DispatchEvent>());
  }

  hashmap<FrameworkID, Framework> frameworks;

  struct Slave
  {
    // Total amount of regular *and* oversubscribed resources.
    Resources total;

    // Regular *and* oversubscribed resources that are allocated.
    //
    // NOTE: We keep track of slave's allocated resources despite
    // having that information in sorters. This is because the
    // information in sorters is not accurate if some framework
    // hasn't reregistered. See MESOS-2919 for details.
    Resources allocated;

    // We track the total and allocated resources on the slave, the
    // available resources are computed as follows:
    //
    //   available = total - allocated
    //
    // Note that it's possible for the slave to be over-allocated!
    // In this case, allocated > total.

    bool activated;  // Whether to offer resources.
    bool checkpoint; // Whether slave supports checkpointing.

    std::string hostname;

    // Represents a scheduled unavailability due to maintenance for a specific
    // slave, and the responses from frameworks as to whether they will be able
    // to gracefully handle this unavailability.
    // NOTE: We currently implement maintenance in the allocator to be able to
    // leverage state and features such as the FrameworkSorter and OfferFilter.
    struct Maintenance
    {
      Maintenance(const Unavailability& _unavailability)
        : unavailability(_unavailability) {}

      // The start time and optional duration of the event.
      Unavailability unavailability;

      // A mapping of frameworks to the inverse offer status associated with
      // this unavailability.
      // NOTE: We currently lose this information during a master fail over
      // since it is not persisted or replicated. This is ok as the new master's
      // allocator will send out new inverse offers and re-collect the
      // information. This is similar to all the outstanding offers from an old
      // master being invalidated, and new offers being sent out.
      hashmap<FrameworkID, mesos::master::InverseOfferStatus> statuses;

      // Represent the "unit of accounting" for maintenance. When a
      // `FrameworkID` is present in the hashset it means an inverse offer has
      // been sent out. When it is not present it means no offer is currently
      // outstanding.
      hashset<FrameworkID> offersOutstanding;
    };

    // When the `maintenance` is set the slave is scheduled to be unavailable at
    // a given point in time, for an optional duration. This information is used
    // to send out `InverseOffers`.
    Option<Maintenance> maintenance;
  };

  hashmap<SlaveID, Slave> slaves;

  hashmap<std::string, mesos::master::RoleInfo> roles;

  // Slaves to send offers for.
  Option<hashset<std::string>> whitelist;

  // There are two levels of sorting, hence "hierarchical".
  // Level 1 sorts across roles:
  //   Reserved resources are excluded from fairness calculation,
  //   since they are forcibly pinned to a role.
  // Level 2 sorts across frameworks within a particular role:
  //   Both reserved resources and unreserved resources are used
  //   in the fairness calculation. This is because reserved
  //   resources can be allocated to any framework in the role.
  //
  // Note that the hierarchical allocator considers oversubscribed
  // resources as regular resources when doing fairness calculations.
  // TODO(vinod): Consider using a different fairness algorithm for
  // oversubscribed resources.
  RoleSorter* roleSorter;
  hashmap<std::string, FrameworkSorter*> frameworkSorters;
};


// Used to represent "filters" for resources unused in offers.
class OfferFilter
{
public:
  virtual ~OfferFilter() {}

  virtual bool filter(const Resources& resources) = 0;
};


class RefusedOfferFilter: public OfferFilter
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
class RefusedInverseOfferFilter: public InverseOfferFilter
{
public:
  RefusedInverseOfferFilter(const process::Timeout& _timeout)
    : timeout(_timeout) {}

  virtual bool filter()
  {
    // See comment above why we currently don't do more fine-grained filtering.
    return timeout.remaining() > Seconds(0);
  }

  const process::Timeout timeout;
};


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::initialize(
    const Duration& _allocationInterval,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, Resources>&)>& _offerCallback,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, UnavailableResources>&)>&
      _inverseOfferCallback,
    const hashmap<std::string, mesos::master::RoleInfo>& _roles)
{
  allocationInterval = _allocationInterval;
  offerCallback = _offerCallback;
  inverseOfferCallback = _inverseOfferCallback;
  roles = _roles;
  initialized = true;

  roleSorter = new RoleSorter();
  foreachpair (
      const std::string& name, const mesos::master::RoleInfo& roleInfo, roles) {
    roleSorter->add(name, roleInfo.weight());
    frameworkSorters[name] = new FrameworkSorter();
  }

  if (roleSorter->count() == 0) {
    LOG(ERROR) << "No roles specified, cannot allocate resources!";
  }

  VLOG(1) << "Initialized hierarchical allocator process";

  delay(allocationInterval, self(), &Self::batch);
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const hashmap<SlaveID, Resources>& used)
{
  CHECK(initialized);

  const std::string& role = frameworkInfo.role();

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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::removeFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  CHECK(frameworks.contains(frameworkId));
  const std::string& role = frameworks[frameworkId].role;

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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::activateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  CHECK(frameworks.contains(frameworkId));
  const std::string& role = frameworks[frameworkId].role;

  frameworkSorters[role]->activate(frameworkId.value());

  LOG(INFO) << "Activated framework " << frameworkId;

  allocate();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::deactivateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  CHECK(frameworks.contains(frameworkId));
  const std::string& role = frameworks[frameworkId].role;

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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateFramework(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::addSlave(
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
      const std::string& role = frameworks[frameworkId].role;

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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::removeSlave(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateSlave(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::activateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].activated = true;

  LOG(INFO)<< "Slave " << slaveId << " reactivated";
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::deactivateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].activated = false;

  LOG(INFO) << "Slave " << slaveId << " deactivated";
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateWhitelist(
    const Option<hashset<std::string>>& _whitelist)
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::requestResources(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateAllocation(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const std::vector<Offer::Operation>& operations)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));
  CHECK(frameworks.contains(frameworkId));

  // Here we apply offer operations to the allocated resources, which
  // in turns leads to an update of the total. The available resources
  // remain unchanged.

  // Update the allocated resources.
  FrameworkSorter* frameworkSorter =
    frameworkSorters[frameworks[frameworkId].role];

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
      frameworks[frameworkId].role,
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


template <class RoleSorter, class FrameworkSorter>
process::Future<Nothing>
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateAvailable(
    const SlaveID& slaveId,
    const std::vector<Offer::Operation>& operations)
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
    return process::Failure(updatedAvailable.error());
  }

  // Update the total resources.
  Try<Resources> updatedTotal = slaves[slaveId].total.apply(operations);
  CHECK_SOME(updatedTotal);

  slaves[slaveId].total = updatedTotal.get();

  // Now, update the total resources in the role sorter.
  roleSorter->update(slaveId, slaves[slaveId].total.unreserved());

  return Nothing();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateUnavailability(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateInverseOffer(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Option<UnavailableResources>& unavailableResources,
    const Option<mesos::master::InverseOfferStatus>& status,
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
      CHECK_NE(
          status.get().status(),
          mesos::master::InverseOfferStatus::UNKNOWN);

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
      new RefusedInverseOfferFilter(process::Timeout::in(seconds.get()));

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


template <class RoleSorter, class FrameworkSorter>
process::Future<
    hashmap<SlaveID, hashmap<FrameworkID, mesos::master::InverseOfferStatus>>>
HierarchicalAllocatorProcess<
    RoleSorter, FrameworkSorter>::getInverseOfferStatuses()
{
  CHECK(initialized);

  hashmap<
      SlaveID,
      hashmap<FrameworkID, mesos::master::InverseOfferStatus>> result;

  // Make a copy of the most recent statuses.
  foreachpair (const SlaveID& id, const Slave& slave, slaves) {
    if (slave.maintenance.isSome()) {
      result[id] = slave.maintenance.get().statuses;
    }
  }

  return result;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::recoverResources(
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
    const std::string& role = frameworks[frameworkId].role;

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
    // NOTE: We cannot add the following CHECK due to the double
    // precision errors. See MESOS-1187 for details.
    // CHECK(slaves[slaveId].allocated.contains(resources));

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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::suppressOffers(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);
  frameworks[frameworkId].suppressed = true;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::reviveOffers(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::batch()
{
  allocate();
  delay(allocationInterval, self(), &Self::batch);
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocate()
{
  Stopwatch stopwatch;
  stopwatch.start();

  allocate(slaves.keys());

  VLOG(1) << "Performed allocation for " << slaves.size() << " slaves in "
            << stopwatch.elapsed();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocate(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocate(
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
  std::vector<SlaveID> slaveIds(slaveIds_.begin(), slaveIds_.end());
  std::random_shuffle(slaveIds.begin(), slaveIds.end());

  foreach (const SlaveID& slaveId, slaveIds) {
    // Don't send offers for non-whitelisted and deactivated slaves.
    if (!isWhitelisted(slaveId) || !slaves[slaveId].activated) {
      continue;
    }

    foreach (const std::string& role, roleSorter->sort()) {
      foreach (const std::string& frameworkId_,
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::deallocate(
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

  foreachvalue (FrameworkSorter* frameworkSorter, frameworkSorters) {
    foreach (const SlaveID& slaveId, slaveIds_) {
      CHECK(slaves.contains(slaveId));

      if (slaves[slaveId].maintenance.isSome()) {
        // We use a reference by alias because we intend to modify the
        // `maintenance` and to improve readability.
        typename Slave::Maintenance& maintenance =
          slaves[slaveId].maintenance.get();

        hashmap<std::string, Resources> allocation =
          frameworkSorter->allocation(slaveId);

        foreachkey (const std::string& frameworkId_, allocation) {
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::expire(
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


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::expire(
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


template <class RoleSorter, class FrameworkSorter>
bool
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::isWhitelisted(
    const SlaveID& slaveId)
{
  CHECK(slaves.contains(slaveId));

  return whitelist.isNone() ||
         whitelist.get().contains(slaves[slaveId].hostname);
}


template <class RoleSorter, class FrameworkSorter>
bool
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::isFiltered(
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


template <class RoleSorter, class FrameworkSorter>
bool HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::isFiltered(
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


template <class RoleSorter, class FrameworkSorter>
bool
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocatable(
    const Resources& resources)
{
  Option<double> cpus = resources.cpus();
  Option<Bytes> mem = resources.mem();

  return (cpus.isSome() && cpus.get() >= MIN_CPUS) ||
         (mem.isSome() && mem.get() >= MIN_MEM);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__
