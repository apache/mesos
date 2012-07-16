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

#include <algorithm>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/timer.hpp>

#include <stout/foreach.hpp>

#include "logging/logging.hpp"

#include "master/dominant_share_allocator.hpp"

using std::sort;
using std::string;
using std::vector;


namespace mesos {
namespace internal {
namespace master {

// Used to represent "filters" for resources unused in offers.
class Filter
{
public:
  virtual ~Filter() {}
  virtual bool filter(const SlaveID& slaveId, const Resources& resources) = 0;
};

class RefusedFilter : public Filter
{
public:
  RefusedFilter(const SlaveID& _slaveId,
                const Resources& _resources,
                const Timeout& _timeout)
    : slaveId(_slaveId),
      resources(_resources),
      timeout(_timeout) {}

  virtual bool filter(const SlaveID& slaveId, const Resources& resources)
  {
    return slaveId == this->slaveId &&
      resources <= this->resources &&
      timeout.remaining() < 0.0;
  }

  const SlaveID slaveId;
  const Resources resources;
  const Timeout timeout;
};


struct DominantShareComparator
{
  DominantShareComparator(const Resources& _resources,
                          const hashmap<FrameworkID, Resources>& _allocated)
    : resources(_resources),
      allocated(_allocated)
  {}

  bool operator () (const FrameworkID& frameworkId1,
                    const FrameworkID& frameworkId2)
  {
    double share1 = 0;
    double share2 = 0;

    // TODO(benh): This implementaion of "dominant resource fairness"
    // currently does not take into account resources that are not
    // scalars.

    foreach (const Resource& resource, resources) {
      if (resource.type() == Value::SCALAR) {
        double total = resource.scalar().value();

        if (total > 0) {
          Value::Scalar none;
          const Value::Scalar& scalar1 =
            allocated[frameworkId1].get(resource.name(), none);
          const Value::Scalar& scalar2 =
            allocated[frameworkId2].get(resource.name(), none);
          share1 = std::max(share1, scalar1.value() / total);
          share2 = std::max(share2, scalar2.value() / total);
        }
      }
    }

    if (share1 == share2) {
      // Make the sort deterministic for unit testing.
      return frameworkId1 < frameworkId2;
    } else {
      return share1 < share2;
    }
  }

  const Resources resources;
  hashmap<FrameworkID, Resources> allocated; // Not const for the '[]' operator.
};


void DominantShareAllocator::initialize(const process::PID<Master>& _master)
{
  master = _master;
  initialized = true;

  // TODO(benh): Consider running periodic allocations. This will
  // definitely be necessary for frameworks that hoard resources (in
  // offers), since otherwise we'll just sit waiting.
  // delay(1.0, self(), &DominantShareAllocator::allocate);
}


void DominantShareAllocator::frameworkAdded(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  CHECK(initialized);

  // Either a framework is being added for the first time or it's
  // being "re-added" (e.g., due to failover). If it's being added for
  // the first time it's possible that it has some resources already
  // allocated to it (e.g., because it just registered with a new
  // master even though the slaves have already registered).
  if (!frameworks.contains(frameworkId)) {
    CHECK(!allocated.contains(frameworkId));
    allocated[frameworkId] = used;
  }

  // We always update the framework info, even if we already had some
  // state about the framework. TODO(benh): Consider adding a check
  // which confirms that the resources used are a subset or equal to
  // the resources allocated (a subset because the allocator might
  // have just asked the master to offer some more resources).
  frameworks[frameworkId] = frameworkInfo;

  LOG(INFO) << "Added framework " << frameworkId;

  allocate();
}


void DominantShareAllocator::frameworkDeactivated(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  frameworks.erase(frameworkId);

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


void DominantShareAllocator::frameworkRemoved(const FrameworkID& frameworkId)
{
  CHECK(initialized);

  // Might not be in 'frameworks' because it was previously
  // deactivated and never re-added.

  frameworks.erase(frameworkId);

  allocated.erase(frameworkId);

  foreach (Filter* filter, filters.get(frameworkId)) {
    filters.remove(frameworkId, filter);
    delete filter;
  }

  filters.remove(frameworkId);

  LOG(INFO) << "Removed framework " << frameworkId;

  allocate();
}


void DominantShareAllocator::slaveAdded(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);

  CHECK(!slaves.contains(slaveId));

  slaves[slaveId] = slaveInfo;

  resources += slaveInfo.resources();

  Resources unused = slaveInfo.resources();

  foreachpair (const FrameworkID& frameworkId, const Resources& resources, used) {
    if (frameworks.contains(frameworkId)) {
      allocated[frameworkId] += resources;
    }
    unused -= resources; // Only want to allocate resources that are not used!
  }

  allocatable[slaveId] = unused;

  LOG(INFO) << "Added slave " << slaveId << " (" << slaveInfo.hostname()
            << ") with " << slaveInfo.resources()
            << " (and " << unused << " available)";

  allocate(slaveId);
}


void DominantShareAllocator::slaveRemoved(const SlaveID& slaveId)
{
  CHECK(initialized);

  CHECK(slaves.contains(slaveId));

  resources -= slaves[slaveId].resources();

  slaves.erase(slaveId);

  allocatable.erase(slaveId);

  // Note that we DO NOT actually delete any filters associated with
  // this slave, that will occur when the delayed
  // DominantShareAllocator::expire gets invoked (or the framework
  // that applied the filters gets removed).

  LOG(INFO) << "Removed slave " << slaveId;
}


void DominantShareAllocator::updateWhitelist(
    const Option<hashset<string> >& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated slave white list:";
    foreach (const string& hostname, whitelist.get()) {
      LOG(INFO) << "\t" << hostname;
    }
  }
}


void DominantShareAllocator::resourcesRequested(
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


void DominantShareAllocator::resourcesUnused(
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

  // Updated resources allocated to framework.
  CHECK(allocated.contains(frameworkId));
  allocated[frameworkId] -= resources;

  // Update resources allocatable on slave.
  CHECK(allocatable.contains(slaveId));
  allocatable[slaveId] += resources;

  // Create a refused resources filter.
  double timeout = filters.isSome()
    ? filters.get().refuse_seconds()
    : Filters().refuse_seconds();

  if (timeout != 0.0) {
    LOG(INFO) << "Framework " << frameworkId
	      << " filtered slave " << slaveId
	      << " for " << timeout << " seconds";

    // Create a new filter and delay it's expiration.
    Filter* filter = new RefusedFilter(slaveId, resources, timeout);
    this->filters.put(frameworkId, filter);

    // TODO(benh): Use 'this' and '&This::' as appropriate.
    delay(timeout, PID<DominantShareAllocator>(this), &DominantShareAllocator::expire,
	  frameworkId, filter);
  }

  allocate(slaveId);
}


void DominantShareAllocator::resourcesRecovered(
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
  // Master::offer before we received Allocator::frameworkRemoved or
  // Allocator::frameworkDeactivated).
  if (allocated.contains(frameworkId)) {
    allocated[frameworkId] -= resources;
  }

  // Update resources allocatable on slave (if slave still exists,
  // which it might not in the event that we dispatched Master::offer
  // before we received Allocator::slaveRemoved).
  if (allocatable.contains(slaveId)) {
    allocatable[slaveId] += resources;

    VLOG(1) << "Recovered " << resources.allocatable()
            << " on slave " << slaveId
            << " from framework " << frameworkId;

    allocate(slaveId);
  }
}


void DominantShareAllocator::offersRevived(const FrameworkID& frameworkId)
{
  CHECK(initialized);

  // TODO(benh): We don't actually delete each Filter right now
  // because that should happen when DominantShareAllocator::expire
  // gets invoked. If we delete the Filter here it's possible that the
  // same Filter (i.e., same address) could get reused and
  // DominantShareAllocator::expire would expire that filter too
  // soon. Note that this only works right now because ALL Filter
  // types "expire".

  foreach (Filter* filter, filters.get(frameworkId)) {
    filters.remove(frameworkId, filter);
    delete filter;
  }

  filters.remove(frameworkId);

  LOG(INFO) << "Removed filters for framework " << frameworkId;

  allocate();
}


void DominantShareAllocator::allocate()
{
  CHECK(initialized);

  allocate(slaves.keys());
}


void DominantShareAllocator::allocate(const SlaveID& slaveId)
{
  CHECK(initialized);

  hashset<SlaveID> slaveIds;
  slaveIds.insert(slaveId);

  allocate(slaveIds);
}


void DominantShareAllocator::allocate(const hashset<SlaveID>& slaveIds)
{
  CHECK(initialized);

  // Order frameworks by dominant resource fairness.
  if (frameworks.size() == 0) {
    VLOG(1) << "No frameworks to allocate resources!";
    return;
  }

  vector<FrameworkID> frameworkIds;

  foreachkey (const FrameworkID& frameworkId, frameworks) {
    frameworkIds.push_back(frameworkId);
  }

  DominantShareComparator comparator(resources, allocated);
  sort(frameworkIds.begin(), frameworkIds.end(), comparator);

  // Get out only "available" resources (i.e., resources that are
  // allocatable and above a certain threshold, see below).
  hashmap<SlaveID, Resources> available;
  foreachpair (const SlaveID& slaveId, Resources resources, allocatable) {
    if (isWhitelisted(slaveId)) {
      resources = resources.allocatable(); // Make sure they're allocatable.

      // TODO(benh): For now, only make offers when there is some cpu
      // and memory left. This is an artifact of the original code
      // that only offered when there was at least 1 cpu "unit"
      // available, and without doing this a framework might get
      // offered resources with only memory available (which it
      // obviously will decline) and then end up waiting the default
      // Filters::refuse_seconds (unless the framework set it to
      // something different).

      Value::Scalar none;
      Value::Scalar cpus = resources.get("cpus", none);
      Value::Scalar mem = resources.get("mem", none);

      if (cpus.value() >= MIN_CPUS && mem.value() > MIN_MEM) {
        VLOG(1) << "Found available resources: " << resources
                << " on slave " << slaveId;
        available[slaveId] = resources;
      }
    }
  }

  if (available.size() == 0) {
    VLOG(1) << "No resources available to allocate!";
    return;
  }

  foreach (const FrameworkID& frameworkId, frameworkIds) {
    // Check if we should offer resources to this framework.
    hashmap<SlaveID, Resources> offerable;
    foreachpair (const SlaveID& slaveId, const Resources& resources, available) {
      // Check whether or not this framework filters this slave.
      bool filtered = false;

      foreach (Filter* filter, filters.get(frameworkId)) {
        if (filter->filter(slaveId, resources)) {
          VLOG(1) << "Filtered " << resources
                  << " on slave " << slaveId
                  << " for framework " << frameworkId;
          filtered = true;
          break;
        }
      }

      if (!filtered) {
        VLOG(1) << "Offering " << resources
                << " on slave " << slaveId
                << " to framework " << frameworkId;
        offerable[slaveId] = resources;

        // Update framework and slave resources.
        allocated[frameworkId] += resources;
        allocatable[slaveId] -= resources;
      }
    }

    if (offerable.size() > 0) {
      foreachkey (const SlaveID& slaveId, offerable) {
        available.erase(slaveId);
      }
      dispatch(master, &Master::offer, frameworkId, offerable);
    }
  }
}


void DominantShareAllocator::expire(
    const FrameworkID& frameworkId,
    Filter* filter)
{
  // Framework might have been removed, in which case it's filters
  // should also already have been deleted.
  if (frameworks.contains(frameworkId)) {
    // Check and see if the filter was already removed in
    // DominantShareAllocator::offersRevived (but not deleted).
    if (filters.contains(frameworkId, filter)) {
      filters.remove(frameworkId, filter);
      delete filter;
      allocate();
    } else {
      delete filter;
    }
  }
}


bool DominantShareAllocator::isWhitelisted(const SlaveID& slaveId)
{
  CHECK(initialized);

  CHECK(slaves.contains(slaveId));

  return whitelist.isNone() ||
    whitelist.get().contains(slaves[slaveId].hostname());
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
