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

#ifndef __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__
#define __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__

#include <mesos/resources.hpp>

#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>

#include "master/allocator.hpp"
#include "master/drf_sorter.hpp"
#include "master/master.hpp"
#include "master/sorter.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// Forward declarations.
class Filter;


// We forward declare the hierarchical allocator process so that we
// can typedef an instantiation of it with DRF sorters.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess;

typedef HierarchicalAllocatorProcess<DRFSorter, DRFSorter>
HierarchicalDRFAllocatorProcess;


struct Slave
{
  Slave() {}

  explicit Slave(const SlaveInfo& _info)
    : available(_info.resources()),
      connected(true),
      whitelisted(false),
      checkpoint(_info.checkpoint()),
      info(_info) {}

  Resources resources() const { return info.resources(); }

  std::string hostname() const { return info.hostname(); }

  // Contains all of the resources currently free on this slave.
  Resources available;

  // Whether the slave is connected. Resources are not offered for
  // disconnected slaves until they reconnect.
  bool connected;

  // Indicates if the resources on this slave should be offered to
  // frameworks.
  bool whitelisted;

  bool checkpoint;
private:
  SlaveInfo info;
};


struct Framework
{
  Framework() {}

  explicit Framework(const FrameworkInfo& _info)
    : checkpoint(_info.checkpoint()),
      info(_info) {}

  std::string role() const { return info.role(); }

  // Filters that have been added by this framework.
  hashset<Filter*> filters;

  bool checkpoint;
private:
  FrameworkInfo info;
};


// Implements the basic allocator algorithm - first pick a role by
// some criteria, then pick one of their frameworks to allocate to.
template <typename RoleSorter, typename FrameworkSorter>
class HierarchicalAllocatorProcess : public AllocatorProcess
{
public:
  HierarchicalAllocatorProcess();

  virtual ~HierarchicalAllocatorProcess();

  process::PID<HierarchicalAllocatorProcess> self();

  void initialize(
      const Flags& flags,
      const process::PID<Master>& _master,
      const hashmap<std::string, RoleInfo>& _roles);

  void frameworkAdded(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used);

  void frameworkRemoved(
      const FrameworkID& frameworkId);

  void frameworkActivated(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo);

  void frameworkDeactivated(
      const FrameworkID& frameworkId);

  void slaveAdded(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const hashmap<FrameworkID, Resources>& used);

  void slaveRemoved(
      const SlaveID& slaveId);

  void slaveDisconnected(
      const SlaveID& slaveId);

  void slaveReconnected(
      const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string> >& whitelist);

  void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void resourcesUnused(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources);

  void offersRevived(
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

  // Remove a filter for the specified framework.
  void expire(const FrameworkID& frameworkId, Filter* filter);

  // Checks whether the slave is whitelisted.
  bool isWhitelisted(const SlaveID& slave);

  // Returns true if there is a filter for this framework
  // on this slave.
  bool isFiltered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources);

  bool allocatable(const Resources& resources);

  bool initialized;

  Flags flags;
  process::PID<Master> master;

  // Contains all frameworks.
  hashmap<FrameworkID, Framework> frameworks;

  // Maps role names to the Sorter object which contains
  // all of that role's frameworks.
  hashmap<std::string, FrameworkSorter*> sorters;

  // Contains all active slaves.
  hashmap<SlaveID, Slave> slaves;

  hashmap<std::string, RoleInfo> roles;

  // Slaves to send offers for.
  Option<hashset<std::string> > whitelist;

  // Sorter containing all active roles.
  RoleSorter* roleSorter;
};


// Used to represent "filters" for resources unused in offers.
class Filter
{
public:
  virtual ~Filter() {}

  virtual bool filter(const SlaveID& slaveId, const Resources& resources) = 0;
};


class RefusedFilter: public Filter
{
public:
  RefusedFilter(
      const SlaveID& _slaveId,
      const Resources& _resources,
      const process::Timeout& _timeout)
    : slaveId(_slaveId), resources(_resources), timeout(_timeout) {}

  virtual bool filter(const SlaveID& slaveId, const Resources& resources)
  {
    return slaveId == this->slaveId &&
           resources <= this->resources && // Refused resources are superset.
           timeout.remaining() > Seconds(0);
  }

  const SlaveID slaveId;
  const Resources resources;
  const process::Timeout timeout;
};


template <class RoleSorter, class FrameworkSorter>
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::HierarchicalAllocatorProcess() // NOLINT(whitespace/line_length)
  : ProcessBase(process::ID::generate("hierarchical-allocator")),
    initialized(false) {}


template <class RoleSorter, class FrameworkSorter>
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::~HierarchicalAllocatorProcess() // NOLINT(whitespace/line_length)
{}


template <class RoleSorter, class FrameworkSorter>
process::PID<HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter> >
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::self()
{
  return process::PID<HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter> >(this); // NOLINT(whitespace/line_length)
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::initialize(
    const Flags& _flags,
    const process::PID<Master>& _master,
    const hashmap<std::string, RoleInfo>& _roles)
{
  flags = _flags;
  master = _master;
  roles = _roles;
  initialized = true;

  roleSorter = new RoleSorter();
  foreachpair (const std::string& name, const RoleInfo& roleInfo, roles) {
    roleSorter->add(name, roleInfo.weight());
    sorters[name] = new FrameworkSorter();
  }

  VLOG(1) << "Initializing hierarchical allocator process "
          << "with master : " << master;

  delay(flags.allocation_interval, self(), &Self::batch);
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::frameworkAdded(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  CHECK(initialized);

  const std::string& role = frameworkInfo.role();

  CHECK(roles.contains(role));

  CHECK(!sorters[role]->contains(frameworkId.value()));
  sorters[role]->add(frameworkId.value());

  // Update the allocation to this framework.
  roleSorter->allocated(role, used);
  sorters[role]->add(used);
  sorters[role]->allocated(frameworkId.value(), used);

  frameworks[frameworkId] = Framework(frameworkInfo);

  LOG(INFO) << "Added framework " << frameworkId;

  allocate();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::frameworkRemoved(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  CHECK(frameworks.contains(frameworkId));
  const std::string& role = frameworks[frameworkId].role();

  // Might not be in 'sorters[role]' because it was previously
  // deactivated and never re-added.
  if (sorters[role]->contains(frameworkId.value())) {
    Resources allocation = sorters[role]->allocation(frameworkId.value());
    roleSorter->unallocated(role, allocation);
    sorters[role]->remove(allocation);
    sorters[role]->remove(frameworkId.value());
  }

  // Do not delete the filters contained in this
  // framework's 'filters' hashset yet, see comments in
  // HierarchicalAllocatorProcess::offersRevived and
  // HierarchicalAllocatorProcess::expire.
  frameworks.erase(frameworkId);

  LOG(INFO) << "Removed framework " << frameworkId;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::frameworkActivated(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  CHECK(initialized);

  const std::string& role = frameworkInfo.role();
  sorters[role]->activate(frameworkId.value());

  LOG(INFO) << "Activated framework " << frameworkId;

  allocate();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::frameworkDeactivated(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  CHECK(frameworks.contains(frameworkId));
  const std::string& role = frameworks[frameworkId].role();

  sorters[role]->deactivate(frameworkId.value());

  // Note that the Sorter *does not* remove the resources allocated
  // to this framework. For now, this is important because if the
  // framework fails over and is activated, we still want a record
  // of the resources that it is using. We might be able to collapse
  // the added/removed and activated/deactivated in the future.

  // Do not delete the filters contained in this
  // framework's 'filters' hashset yet, see comments in
  // HierarchicalAllocatorProcess::offersRevived and
  // HierarchicalAllocatorProcess::expire.
  frameworks[frameworkId].filters.clear();

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::slaveAdded(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);

  CHECK(!slaves.contains(slaveId));

  slaves[slaveId] = Slave(slaveInfo);
  slaves[slaveId].whitelisted = isWhitelisted(slaveId);

  roleSorter->add(slaveInfo.resources());

  Resources unused = slaveInfo.resources();

  foreachpair (const FrameworkID& frameworkId,
               const Resources& resources,
               used) {
    if (frameworks.contains(frameworkId)) {
      const std::string& role = frameworks[frameworkId].role();
      sorters[role]->add(resources);
      sorters[role]->allocated(frameworkId.value(), resources);
      roleSorter->allocated(role, resources);
    }

    unused -= resources; // Only want to allocate resources that are not used!
  }

  slaves[slaveId].available = unused;

  LOG(INFO) << "Added slave " << slaveId << " (" << slaveInfo.hostname()
            << ") with " << slaveInfo.resources() << " (and " << unused
            << " available)";

  allocate(slaveId);
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::slaveRemoved(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  roleSorter->remove(slaves[slaveId].resources());

  slaves.erase(slaveId);

  // Note that we DO NOT actually delete any filters associated with
  // this slave, that will occur when the delayed
  // HierarchicalAllocatorProcess::expire gets invoked (or the framework
  // that applied the filters gets removed).

  LOG(INFO) << "Removed slave " << slaveId;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::slaveDisconnected(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].connected = false;

  LOG(INFO) << "Slave " << slaveId << " disconnected";
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::slaveReconnected(
    const SlaveID& slaveId)
{
  CHECK(initialized);
  CHECK(slaves.contains(slaveId));

  slaves[slaveId].connected = true;

  LOG(INFO)<< "Slave " << slaveId << " reconnected";
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::updateWhitelist(
    const Option<hashset<std::string> >& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated slave white list: " << stringify(whitelist.get());

    foreachkey (const SlaveID& slaveId, slaves) {
      slaves[slaveId].whitelisted = isWhitelisted(slaveId);
    }
  }
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::resourcesRequested(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters)
{
  CHECK(initialized);

  if (resources.allocatable().size() == 0) {
    return;
  }

  VLOG(1) << "Framework " << frameworkId
          << " left " << resources.allocatable()
          << " unused on slave " << slaveId;

  // Update resources allocated to framework. It is
  // not possible for the role to not be in roles
  // because resourcesUnused is only called as the
  // result of a valid task launch by an active
  // framework that doesn't use the entire offer.
  CHECK(frameworks.contains(frameworkId));

  const std::string& role = frameworks[frameworkId].role();
  sorters[role]->unallocated(frameworkId.value(), resources);
  sorters[role]->remove(resources);
  roleSorter->unallocated(role, resources);

  // Update resources allocatable on slave.
  CHECK(slaves.contains(slaveId));
  slaves[slaveId].available += resources;

  // Create a refused resources filter.
  Try<Duration> seconds_ = Duration::create(Filters().refuse_seconds());
  CHECK_SOME(seconds_);
  Duration seconds = seconds_.get();

  // Update the value of 'seconds' if the input isSome() and is
  // valid.
  if (filters.isSome()) {
    seconds_ = Duration::create(filters.get().refuse_seconds());
    if (seconds_.isError()) {
      LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                   << "the refused resources filter because the input value is "
                   << "invalid: " << seconds_.error();
    } else if (seconds_.get() < Duration::zero()) {
      LOG(WARNING) << "Using the default value of 'refuse_seconds' to create "
                   << "the refused resources filter because the input value is "
                   << "negative";
    } else {
      seconds = seconds_.get();
    }
  }

  if (seconds != Duration::zero()) {
    LOG(INFO) << "Framework " << frameworkId
              << " filtered slave " << slaveId
              << " for " << seconds;

    // Create a new filter and delay it's expiration.
    Filter* filter =
      new RefusedFilter(slaveId, resources, process::Timeout::in(seconds));

    frameworks[frameworkId].filters.insert(filter);

    delay(seconds, self(), &Self::expire, frameworkId, filter);
  }
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(initialized);

  if (resources.allocatable().size() == 0) {
    return;
  }

  // Updated resources allocated to framework (if framework still
  // exists, which it might not in the event that we dispatched
  // Master::offer before we received AllocatorProcess::frameworkRemoved
  // or AllocatorProcess::frameworkDeactivated, in which case we will
  // have already recovered all of its resources).
  if (frameworks.contains(frameworkId) &&
      sorters[frameworks[frameworkId].role()]->contains(frameworkId.value())) {
    const std::string& role = frameworks[frameworkId].role();
    sorters[role]->unallocated(frameworkId.value(), resources);
    sorters[role]->remove(resources);
    roleSorter->unallocated(role, resources);
  }

  // Update resources allocatable on slave (if slave still exists,
  // which it might not in the event that we dispatched Master::offer
  // before we received Allocator::slaveRemoved).
  if (slaves.contains(slaveId)) {
    slaves[slaveId].available += resources;

    LOG(INFO) << "Recovered " << resources.allocatable()
              << " (total allocatable: " << slaves[slaveId].available
              << ") on slave " << slaveId
              << " from framework " << frameworkId;
  }
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::offersRevived(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  frameworks[frameworkId].filters.clear();

  // We delete each actual Filter when
  // HierarchicalAllocatorProcess::expire gets invoked. If we delete the
  // Filter here it's possible that the same Filter (i.e., same
  // address) could get reused and HierarchicalAllocatorProcess::expire
  // would expire that filter too soon. Note that this only works
  // right now because ALL Filter types "expire".

  LOG(INFO) << "Removed filters for framework " << frameworkId;

  allocate();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::batch()
{
  CHECK(initialized);
  allocate();
  delay(flags.allocation_interval, self(), &Self::batch);
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocate()
{
  CHECK(initialized);

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
  CHECK(initialized);

  hashset<SlaveID> slaveIds;
  slaveIds.insert(slaveId);

  Stopwatch stopwatch;
  stopwatch.start();

  allocate(slaveIds);

  VLOG(1) << "Performed allocation for slave " << slaveId << " in "
          << stopwatch.elapsed();
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocate(
    const hashset<SlaveID>& slaveIds)
{
  CHECK(initialized);

  if (roleSorter->count() == 0) {
    VLOG(1) << "No roles to allocate resources!";
    return;
  }

  if (slaveIds.empty()) {
    VLOG(1) << "No resources available to allocate!";
    return;
  }

  foreach (const std::string& role, roleSorter->sort()) {
    foreach (const std::string& frameworkIdValue, sorters[role]->sort()) {
      FrameworkID frameworkId;
      frameworkId.set_value(frameworkIdValue);

      Resources allocatedResources;
      hashmap<SlaveID, Resources> offerable;
      foreach (const SlaveID& slaveId, slaveIds) {
        Resources unreserved = slaves[slaveId].available.extract("*");
        Resources resources = unreserved;

        if (role != "*") {
          resources += slaves[slaveId].available.extract(role);
        }

        // Check whether or not this framework filters this slave.
        bool filtered = isFiltered(frameworkId, slaveId, resources);

        if (!filtered &&
            slaves[slaveId].connected &&
            slaves[slaveId].whitelisted &&
            allocatable(resources)) {
          VLOG(1)
            << "Offering " << resources << " on slave " << slaveId
            << " to framework " << frameworkId;

          offerable[slaveId] = resources;

          // Update framework and slave resources.
          slaves[slaveId].available -= resources;

          // We only count resources not reserved for this role
          // in the share the sorter considers.
          allocatedResources += unreserved;
        }
      }

      if (!offerable.empty()) {
        sorters[role]->add(allocatedResources);
        sorters[role]->allocated(frameworkIdValue, allocatedResources);
        roleSorter->allocated(role, allocatedResources);

        dispatch(master, &Master::offer, frameworkId, offerable);
      }
    }
  }
}


template <class RoleSorter, class FrameworkSorter>
void
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::expire(
    const FrameworkID& frameworkId,
    Filter* filter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in
  // HierarchicalAllocatorProcess::offersRevived) but not yet deleted (to
  // keep the address from getting reused possibly causing premature
  // expiration).
  if (frameworks.contains(frameworkId) &&
      frameworks[frameworkId].filters.contains(filter)) {
    frameworks[frameworkId].filters.erase(filter);
  }

  delete filter;
}


template <class RoleSorter, class FrameworkSorter>
bool
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::isWhitelisted(
    const SlaveID& slaveId)
{
  CHECK(initialized);

  CHECK(slaves.contains(slaveId));

  return whitelist.isNone() ||
         whitelist.get().contains(slaves[slaveId].hostname());
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
    VLOG(1) << "Filtered " << resources
            << " on non-checkpointing slave " << slaveId
            << " for checkpointing framework " << frameworkId;
    return true;
  }

  foreach (Filter* filter, frameworks[frameworkId].filters) {
    if (filter->filter(slaveId, resources)) {
      VLOG(1) << "Filtered " << resources
              << " on slave " << slaveId
              << " for framework " << frameworkId;
      return true;
    }
  }
  return false;
}


template <class RoleSorter, class FrameworkSorter>
bool
HierarchicalAllocatorProcess<RoleSorter, FrameworkSorter>::allocatable(
    const Resources& resources)
{
  // TODO(benh): For now, only make offers when there is some cpu
  // and memory left. This is an artifact of the original code that
  // only offered when there was at least 1 cpu "unit" available,
  // and without doing this a framework might get offered resources
  // with only memory available (which it obviously will decline)
  // and then end up waiting the default Filters::refuse_seconds
  // (unless the framework set it to something different).

  Option<double> cpus = resources.cpus();
  Option<Bytes> mem = resources.mem();

  if (cpus.isSome() && mem.isSome()) {
    return cpus.get() >= MIN_CPUS && mem.get() > MIN_MEM;
  }

  return false;
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__
