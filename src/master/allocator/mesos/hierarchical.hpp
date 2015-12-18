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

#ifndef __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__
#define __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>

#include "master/allocator/mesos/allocator.hpp"
#include "master/allocator/sorter/drf/sorter.hpp"

#include "master/constants.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// We forward declare the hierarchical allocator process so that we
// can typedef an instantiation of it with DRF sorters.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess;

typedef HierarchicalAllocatorProcess<DRFSorter, DRFSorter>
HierarchicalDRFAllocatorProcess;

typedef MesosAllocator<HierarchicalDRFAllocatorProcess>
HierarchicalDRFAllocator;


namespace internal {

// Forward declarations.
class OfferFilter;
class InverseOfferFilter;


// Implements the basic allocator algorithm - first pick a role by
// some criteria, then pick one of their frameworks to allocate to.
class HierarchicalAllocatorProcess : public MesosAllocatorProcess
{
public:
  HierarchicalAllocatorProcess(
      const std::function<Sorter*()>& _roleSorterFactory,
      const std::function<Sorter*()>& _frameworkSorterFactory)
    : ProcessBase(process::ID::generate("hierarchical-allocator")),
      initialized(false),
      paused(true),
      metrics(*this),
      roleSorterFactory(_roleSorterFactory),
      frameworkSorterFactory(_frameworkSorterFactory),
      quotaRoleSorter(NULL),
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
      const hashmap<std::string, double>& weights);

  void recover(
      const int _expectedAgentCount,
      const hashmap<std::string, Quota>& quotas);

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

  void setQuota(
      const std::string& role,
      const mesos::quota::QuotaInfo& quota);

  void removeQuota(
      const std::string& role);

protected:
  // Useful typedefs for dispatch/delay/defer to self()/this.
  typedef HierarchicalAllocatorProcess Self;
  typedef HierarchicalAllocatorProcess This;

  // Helpers for pausing and resuming allocation.
  void pause();
  void resume();

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

  // Returns the weight of the specified role name.
  double roleWeight(const std::string& name);

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
  bool paused;

  // Recovery data.
  Option<int> expectedAgentCount;

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

      // Represents the "unit of accounting" for maintenance. When a
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

  // Number of registered frameworks for each role. When a role's active
  // count drops to zero, it is removed from this map; the role is also
  // removed from `roleSorter` and its `frameworkSorter` is deleted.
  hashmap<std::string, int> activeRoles;

  // Configured weight for each role, if any; if a role does not
  // appear here, it has the default weight of 1.
  hashmap<std::string, double> weights;

  // Configured quota for each role, if any. Setting quota for a role
  // changes the order that the role's frameworks are offered
  // resources. Quota comes before fair share, hence setting quota moves
  // the role's frameworks towards the front of the allocation queue.
  //
  // NOTE: We currently associate quota with roles, but this may
  // change in the future.
  hashmap<std::string, mesos::quota::QuotaInfo> quotas;

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
  const std::function<Sorter*()> roleSorterFactory;
  const std::function<Sorter*()> frameworkSorterFactory;

  // A dedicated sorter for roles for which quota is set. Quota'ed roles
  // belong to an extra allocation group and have resources allocated up
  // to their alloted quota prior to non-quota'ed roles.
  //
  // Note that a role appears in `quotaRoleSorter` if it has a quota
  // (even if no frameworks are currently registered in that role). In
  // contrast, `roleSorter` only contains entries for roles with one or
  // more registered frameworks.
  //
  // NOTE: This sorter counts only unreserved non-revocable resources.
  // TODO(alexr): Consider including dynamically reserved resources.
  Sorter* quotaRoleSorter;

  Sorter* roleSorter;
  hashmap<std::string, Sorter*> frameworkSorters;
};


} // namespace internal {


// We map the templatized version of the `HierarchicalAllocatorProcess` to one
// that relies on sorter factories in the internal namespace. This allows us
// to keep the implemention of the allocator in the implementation file.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess
  : public internal::HierarchicalAllocatorProcess
{
public:
  HierarchicalAllocatorProcess()
    : internal::HierarchicalAllocatorProcess(
          []() -> Sorter* { return new RoleSorter(); },
          []() -> Sorter* { return new FrameworkSorter(); }) {}
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__
