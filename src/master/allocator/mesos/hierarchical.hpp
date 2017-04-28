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

#include <set>
#include <string>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

#include "common/protobuf_utils.hpp"

#include "master/allocator/mesos/allocator.hpp"
#include "master/allocator/mesos/metrics.hpp"

#include "master/allocator/sorter/drf/sorter.hpp"

#include "master/constants.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// We forward declare the hierarchical allocator process so that we
// can typedef an instantiation of it with DRF sorters.
template <
    typename RoleSorter,
    typename FrameworkSorter,
    typename QuotaRoleSorter>
class HierarchicalAllocatorProcess;

typedef HierarchicalAllocatorProcess<DRFSorter, DRFSorter, DRFSorter>
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
      const std::function<Sorter*()>& roleSorterFactory,
      const std::function<Sorter*()>& _frameworkSorterFactory,
      const std::function<Sorter*()>& quotaRoleSorterFactory)
    : initialized(false),
      paused(true),
      metrics(*this),
      roleSorter(roleSorterFactory()),
      quotaRoleSorter(quotaRoleSorterFactory()),
      frameworkSorterFactory(_frameworkSorterFactory) {}

  virtual ~HierarchicalAllocatorProcess() {}

  process::PID<HierarchicalAllocatorProcess> self() const
  {
    return process::PID<Self>(this);
  }

  void initialize(
      const Duration& allocationInterval,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&
        offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback,
      const Option<std::set<std::string>>&
        fairnessExcludeResourceNames = None());

  void recover(
      const int _expectedAgentCount,
      const hashmap<std::string, Quota>& quotas);

  void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used,
      bool active);

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
      const std::vector<SlaveInfo::Capability>& capabilities,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used);

  void removeSlave(
      const SlaveID& slaveId);

  void updateSlave(
      const SlaveID& slave,
      const Option<Resources>& oversubscribed = None(),
      const Option<std::vector<SlaveInfo::Capability>>& capabilities = None());

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
      const Resources& offeredResources,
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
      const Option<mesos::allocator::InverseOfferStatus>& status,
      const Option<Filters>& filters);

  process::Future<
      hashmap<SlaveID,
      hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>>>
    getInverseOfferStatuses();

  void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void suppressOffers(
      const FrameworkID& frameworkId,
      const std::set<std::string>& roles);

  void reviveOffers(
      const FrameworkID& frameworkId,
      const std::set<std::string>& roles);

  void setQuota(
      const std::string& role,
      const Quota& quota);

  void removeQuota(
      const std::string& role);

  void updateWeights(
      const std::vector<WeightInfo>& weightInfos);

protected:
  // Useful typedefs for dispatch/delay/defer to self()/this.
  typedef HierarchicalAllocatorProcess Self;
  typedef HierarchicalAllocatorProcess This;

  // Idempotent helpers for pausing and resuming allocation.
  void pause();
  void resume();

  // Callback for doing batch allocations.
  void batch();

  // Allocate any allocatable resources from all known agents.
  process::Future<Nothing> allocate();

  // Allocate resources from the specified agent.
  process::Future<Nothing> allocate(const SlaveID& slaveId);

  // Allocate resources from the specified agents. The allocation
  // is deferred and batched with other allocation requests.
  process::Future<Nothing> allocate(const hashset<SlaveID>& slaveIds);

  // Method that performs allocation work.
  Nothing _allocate();

  // Helper for `_allocate()` that allocates resources for offers.
  void __allocate();

  // Helper for `_allocate()` that deallocates resources for inverse offers.
  void deallocate();

  // Remove an offer filter for the specified role of the framework.
  void expire(
      const FrameworkID& frameworkId,
      const std::string& role,
      const SlaveID& slaveId,
      OfferFilter* offerFilter);

  void _expire(
      const FrameworkID& frameworkId,
      const std::string& role,
      const SlaveID& slaveId,
      OfferFilter* offerFilter);

  // Remove an inverse offer filter for the specified framework.
  void expire(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      InverseOfferFilter* inverseOfferFilter);

  // Checks whether the slave is whitelisted.
  bool isWhitelisted(const SlaveID& slaveId) const;

  // Returns true if there is a resource offer filter for the
  // specified role of this framework on this slave.
  bool isFiltered(
      const FrameworkID& frameworkId,
      const std::string& role,
      const SlaveID& slaveId,
      const Resources& resources) const;

  // Returns true if there is an inverse offer filter for this framework
  // on this slave.
  bool isFiltered(
      const FrameworkID& frameworkID,
      const SlaveID& slaveID) const;

  static bool allocatable(const Resources& resources);

  bool initialized;
  bool paused;

  // Recovery data.
  Option<int> expectedAgentCount;

  Duration allocationInterval;

  lambda::function<
      void(const FrameworkID&,
           const hashmap<std::string, hashmap<SlaveID, Resources>>&)>
    offerCallback;

  lambda::function<
      void(const FrameworkID&,
           const hashmap<SlaveID, UnavailableResources>&)>
    inverseOfferCallback;

  friend Metrics;
  Metrics metrics;

  struct Framework
  {
    explicit Framework(const FrameworkInfo& frameworkInfo);

    std::set<std::string> roles;

    protobuf::framework::Capabilities capabilities;

    // Active offer and inverse offer filters for the framework.
    // Offer filters are tied to the role the filtered resources
    // were allocated to.
    hashmap<std::string, hashmap<SlaveID, hashset<OfferFilter*>>> offerFilters;
    hashmap<SlaveID, hashset<InverseOfferFilter*>> inverseOfferFilters;
  };

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<process::DispatchEvent>());
  }

  double _resources_total(
      const std::string& resource);

  double _resources_offered_or_allocated(
      const std::string& resource);

  double _quota_allocated(
      const std::string& role,
      const std::string& resource);

  double _offer_filters_active(
      const std::string& role);

  hashmap<FrameworkID, Framework> frameworks;

  struct Slave
  {
    // Total amount of regular *and* oversubscribed resources.
    Resources total;

    // Regular *and* oversubscribed resources that are allocated.
    //
    // NOTE: We maintain multiple copies of each shared resource allocated
    // to a slave, where the number of copies represents the number of times
    // this shared resource has been allocated to (and has not been recovered
    // from) a specific framework.
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
    Resources available() const
    {
      // In order to subtract from the total,
      // we strip the allocation information.
      Resources allocated_ = allocated;
      allocated_.unallocate();

      return total - allocated_;
    }

    bool activated;  // Whether to offer resources.

    std::string hostname;

    protobuf::slave::Capabilities capabilities;

    // Represents a scheduled unavailability due to maintenance for a specific
    // slave, and the responses from frameworks as to whether they will be able
    // to gracefully handle this unavailability.
    //
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
      //
      // NOTE: We currently lose this information during a master fail over
      // since it is not persisted or replicated. This is ok as the new master's
      // allocator will send out new inverse offers and re-collect the
      // information. This is similar to all the outstanding offers from an old
      // master being invalidated, and new offers being sent out.
      hashmap<FrameworkID, mesos::allocator::InverseOfferStatus> statuses;

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

  // A set of agents that are kept as allocation candidates. Events
  // may add or remove candidates to the set. When an allocation is
  // processed, the set of candidates is cleared.
  hashset<SlaveID> allocationCandidates;

  // Future for the dispatched allocation that becomes
  // ready after the allocation run is complete.
  Option<process::Future<Nothing>> allocation;

  // We track information about roles that we're aware of in the system.
  // Specifically, we keep track of the roles when a framework subscribes to
  // the role, and/or when there are resources allocated to the role
  // (e.g. some tasks and/or executors are consuming resources under the role).
  hashmap<std::string, hashset<FrameworkID>> roles;

  // Configured quota for each role, if any. Setting quota for a role
  // changes the order that the role's frameworks are offered
  // resources. Quota comes before fair share, hence setting quota moves
  // the role's frameworks towards the front of the allocation queue.
  //
  // NOTE: We currently associate quota with roles, but this may
  // change in the future.
  hashmap<std::string, Quota> quotas;

  // Slaves to send offers for.
  Option<hashset<std::string>> whitelist;

  // Resources (by name) that will be excluded from a role's fair share.
  Option<std::set<std::string>> fairnessExcludeResourceNames;

  // There are two stages of allocation. During the first stage resources
  // are allocated only to frameworks in roles with quota set. During the
  // second stage remaining resources that would not be required to satisfy
  // un-allocated quota are then allocated to all frameworks.
  //
  // Each stage comprises two levels of sorting, hence "hierarchical".
  // Level 1 sorts across roles:
  //   Currently, only the allocated portion of the reserved resources are
  //   accounted for fairness calculation.
  //
  // TODO(mpark): Reserved resources should be accounted for fairness
  // calculation whether they are allocated or not, since they model a long or
  // forever running task. That is, the effect of reserving resources is
  // equivalent to launching a task in that the resources that make up the
  // reservation are not available to other roles as non-revocable.
  //
  // Level 2 sorts across frameworks within a particular role:
  //   Reserved resources at this level are, and should be accounted for
  //   fairness calculation only if they are allocated. This is because
  //   reserved resources are fairly shared across the frameworks in the role.
  //
  // The allocator relies on `Sorter`s to employ a particular sorting
  // algorithm. Each level has its own sorter and hence may have different
  // fairness calculations.
  //
  // NOTE: The hierarchical allocator considers revocable resources as
  // regular resources when doing fairness calculations.
  //
  // TODO(vinod): Consider using a different fairness algorithm for
  // revocable resources.

  // A sorter for active roles. This sorter determines the order in which
  // roles are allocated resources during Level 1 of the second stage.
  process::Owned<Sorter> roleSorter;

  // A dedicated sorter for roles for which quota is set. This sorter
  // determines the order in which quota'ed roles are allocated resources
  // during Level 1 of the first stage. Quota'ed roles have resources
  // allocated up to their alloted quota (the first stage) prior to
  // non-quota'ed roles (the second stage).
  //
  // NOTE: A role appears in `quotaRoleSorter` if it has a quota (even if
  // no frameworks are currently registered in that role). In contrast,
  // `roleSorter` only contains entries for roles with one or more
  // registered frameworks.
  //
  // NOTE: We do not include revocable resources in the quota role sorter,
  // because the quota role sorter's job is to perform fair sharing between
  // the quota roles as it pertains to their level of quota satisfaction.
  // Since revocable resources do not increase a role's level of satisfaction
  // toward its quota, we choose to exclude them from the quota role sorter.
  process::Owned<Sorter> quotaRoleSorter;

  // A collection of sorters, one per active role. Each sorter determines
  // the order in which frameworks that belong to the same role are allocated
  // resources inside the role's share. These sorters are used during Level 2
  // for both the first and the second stages.
  hashmap<std::string, process::Owned<Sorter>> frameworkSorters;

  // Factory function for framework sorters.
  const std::function<Sorter*()> frameworkSorterFactory;

private:
  bool isFrameworkTrackedUnderRole(
      const FrameworkID& frameworkId,
      const std::string& role) const;

  void trackFrameworkUnderRole(
      const FrameworkID& frameworkId,
      const std::string& role);

  void untrackFrameworkUnderRole(
      const FrameworkID& frameworkId,
      const std::string& role);

  // Helper to update the agent's total resources maintained in the allocator
  // and the role and quota sorters (whose total resources match the agent's
  // total resources).
  void updateSlaveTotal(const SlaveID& slaveId, const Resources& total);
};


} // namespace internal {


// We map the templatized version of the `HierarchicalAllocatorProcess` to one
// that relies on sorter factories in the internal namespace. This allows us
// to keep the implementation of the allocator in the implementation file.
template <
    typename RoleSorter,
    typename FrameworkSorter,
    typename QuotaRoleSorter>
class HierarchicalAllocatorProcess
  : public internal::HierarchicalAllocatorProcess
{
public:
  HierarchicalAllocatorProcess()
    : ProcessBase(process::ID::generate("hierarchical-allocator")),
      internal::HierarchicalAllocatorProcess(
          [this]() -> Sorter* {
            return new RoleSorter(this->self(), "allocator/mesos/roles/");
          },
          []() -> Sorter* { return new FrameworkSorter(); },
          []() -> Sorter* { return new QuotaRoleSorter(); }) {}
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_HIERARCHICAL_HPP__
