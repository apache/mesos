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

#include <glog/logging.h>

#include <algorithm>

#include "common/utils.hpp"

#include "master/simple_allocator.hpp"

using std::max;
using std::sort;
using std::vector;


namespace mesos {
namespace internal {
namespace master {

void SimpleAllocator::initialize(Master* _master)
{
  master = _master;
  initialized = true;
}


void SimpleAllocator::frameworkAdded(Framework* framework)
{
  CHECK(initialized);
  LOG(INFO) << "Added framework " << framework->id;
  makeNewOffers();
}


void SimpleAllocator::frameworkRemoved(Framework* framework)
{
  CHECK(initialized);

  foreachkey (const SlaveID& slaveId, utils::copy(refusers)) {
    refusers.remove(slaveId, framework->id);
  }

  LOG(INFO) << "Removed framework " << framework->id;

  makeNewOffers();
}


void SimpleAllocator::slaveAdded(Slave* slave)
{
  CHECK(initialized);

  LOG(INFO) << "Added slave " << slave->id
            << " with " << slave->info.resources();

  totalResources += slave->info.resources();
  makeNewOffers(slave);
}


void SimpleAllocator::slaveRemoved(Slave* slave)
{
  CHECK(initialized);

  LOG(INFO) << "Removed slave " << slave->id;

  totalResources -= slave->info.resources();
  refusers.remove(slave->id);
}


void SimpleAllocator::resourcesRequested(
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


void SimpleAllocator::resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(initialized);

  if (resources.allocatable().size() > 0) {
    VLOG(1) << "Framework " << frameworkId
            << " left " << resources.allocatable()
            << " unused on slave " << slaveId;
    refusers.put(slaveId, frameworkId);
  }

  makeNewOffers();
}


void SimpleAllocator::resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(initialized);

  if (resources.allocatable().size() > 0) {
    VLOG(1) << "Recovered " << resources.allocatable()
            << " on slave " << slaveId
            << " from framework " << frameworkId;
    refusers.remove(slaveId);
  }

  makeNewOffers();
}


void SimpleAllocator::offersRevived(Framework* framework)
{
  CHECK(initialized);

  // TODO(benh): This is broken ... we say filters removed here, but
  // yet we don't actually do the removing of the filters? The
  // allocator should really be responsible for filters (if there are
  // any) because it can best use those for future allocation
  // decisions.
  LOG(INFO) << "Filters removed for framework " << framework->id;

  makeNewOffers();
}


void SimpleAllocator::timerTick()
{
  CHECK(initialized);
  makeNewOffers();
}


namespace {

struct DominantShareComparator
{
  DominantShareComparator(const Resources& _resources)
    : resources(_resources) {}

  bool operator () (Framework* framework1, Framework* framework2)
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
            framework1->resources.get(resource.name(), none);
          const Value::Scalar& scalar2 =
            framework2->resources.get(resource.name(), none);
          share1 = max(share1, scalar1.value() / total);
          share2 = max(share2, scalar2.value() / total);
        }
      }
    }

    if (share1 == share2) {
      // Make the sort deterministic for unit testing.
      return framework1->id.value() < framework2->id.value();
    } else {
      return share1 < share2;
    }
  }

  Resources resources;
};

} // namespace {


vector<Framework*> SimpleAllocator::getAllocationOrdering()
{
  CHECK(initialized) << "Cannot get allocation ordering before initialization!";
  vector<Framework*> frameworks = master->getActiveFrameworks();
  DominantShareComparator comp(totalResources);
  sort(frameworks.begin(), frameworks.end(), comp);
  return frameworks;
}


void SimpleAllocator::makeNewOffers()
{
  // TODO: Create a method in master so that we don't return the whole list of slaves
  CHECK(initialized) << "Cannot make new offers before initialization!";
  vector<Slave*> slaves = master->getActiveSlaves();
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(Slave* slave)
{
  CHECK(initialized) << "Cannot make new offers before initialization!";
  vector<Slave*> slaves;
  slaves.push_back(slave);
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(const vector<Slave*>& slaves)
{
  CHECK(initialized) << "Cannot make new offers before initialization!";
  // Get an ordering of frameworks to send offers to
  vector<Framework*> ordering = getAllocationOrdering();
  if (ordering.empty()) {
    VLOG(1) << "No frameworks to allocate resources!";
    return;
  }

  // Find all the available resources that can be allocated.
  hashmap<Slave*, Resources> available;
  foreach (Slave* slave, slaves) {
    if (slave->active) {
      Resources resources = slave->resourcesFree().allocatable();

      // TODO(benh): For now, only make offers when there is some cpu
      // and memory left. This is an artifact of the original code
      // that only offered when there was at least 1 cpu "unit"
      // available, and without doing this a framework might get
      // offered resources with only memory available (which it
      // obviously won't take) and then get added as a refuser for
      // that slave and therefore have to wait upwards of
      // DEFAULT_REFUSAL_TIMEOUT until resources come from that slave
      // again. In the long run, frameworks will poll the master for
      // resources, rather than the master pushing resources out to
      // frameworks.

      Value::Scalar none;
      Value::Scalar cpus = resources.get("cpus", none);
      Value::Scalar mem = resources.get("mem", none);

      if (cpus.value() >= MIN_CPUS && mem.value() > MIN_MEM) {
        VLOG(1) << "Found available resources: " << resources
                << " on slave " << slave->id;
        available[slave] = resources;
      }
    }
  }

  if (available.size() == 0) {
    VLOG(1) << "No resources available to allocate!";
    return;
  }

  // Clear refusers on any slave that has been refused by everyone.
  foreachkey (Slave* slave, available) {
    if (refusers.get(slave->id).size() == ordering.size()) {
      VLOG(1) << "Clearing refusers for slave " << slave->id
              << " because EVERYONE has refused resources from it";
      refusers.remove(slave->id);
    }
  }

  foreach (Framework* framework, ordering) {
    // Check if we should offer resources to this framework.
    hashmap<Slave*, Resources> offerable;
    foreachpair (Slave* slave, const Resources& resources, available) {
      if (!refusers.contains(slave->id, framework->id) &&
          !framework->filters(slave, resources)) {
        VLOG(1) << "Offering " << resources
                << " on slave " << slave->id
                << " to framework " << framework->id;
        offerable[slave] = resources;
      }
    }

    if (offerable.size() > 0) {
      foreachkey (Slave* slave, offerable) {
        available.erase(slave);
      }

      master->makeOffers(framework, offerable);
    }
  }
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
