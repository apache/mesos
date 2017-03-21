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

#include "master/allocator/sorter/drf/sorter.hpp"

#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

using std::set;
using std::string;
using std::vector;

using process::UPID;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

bool DRFComparator::operator()(const Client& client1, const Client& client2)
{
  if (client1.share == client2.share) {
    if (client1.allocations == client2.allocations) {
      return client1.name < client2.name;
    }
    return client1.allocations < client2.allocations;
  }
  return client1.share < client2.share;
}


DRFSorter::DRFSorter(
    const UPID& allocator,
    const string& metricsPrefix)
  : metrics(Metrics(allocator, *this, metricsPrefix)) {}


void DRFSorter::initialize(
    const Option<set<string>>& _fairnessExcludeResourceNames)
{
  fairnessExcludeResourceNames = _fairnessExcludeResourceNames;
}


void DRFSorter::add(const string& name, double weight)
{
  CHECK(!contains(name));

  Client client(name, 0, 0);
  clients.insert(client);

  allocations[name] = Allocation();
  weights[name] = weight;

  if (metrics.isSome()) {
    metrics->add(name);
  }
}


void DRFSorter::update(const string& name, double weight)
{
  CHECK(weights.contains(name));
  weights[name] = weight;

  // If the total resources have changed, we're going to
  // recalculate all the shares, so don't bother just
  // updating this client.
  if (!dirty) {
    updateShare(name);
  }
}


void DRFSorter::remove(const string& name)
{
  CHECK(contains(name));

  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) {
    clients.erase(it);
  }

  allocations.erase(name);
  weights.erase(name);

  if (metrics.isSome()) {
    metrics->remove(name);
  }
}


void DRFSorter::activate(const string& name)
{
  CHECK(contains(name));

  set<Client, DRFComparator>::iterator it = find(name);
  if (it == clients.end()) {
    Client client(name, calculateShare(name), 0);
    clients.insert(client);
  }
}


void DRFSorter::deactivate(const string& name)
{
  CHECK(contains(name));

  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) {
    // TODO(benh): Removing the client is an unfortunate strategy
    // because we lose information such as the number of allocations
    // for this client which means the fairness can be gamed by a
    // framework disconnecting and reconnecting.
    clients.erase(it);
  }
}


void DRFSorter::allocated(
    const string& name,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(contains(name));

  set<Client, DRFComparator>::iterator it = find(name);

  // The allocator might notify us about an allocation that has been
  // made to an inactive sorter client. For example, this happens when
  // an agent re-registers that is running tasks for a framework that
  // has not yet re-registered.
  if (it != clients.end()) {
    // TODO(benh): Refactor 'updateShare' to be able to reuse it here.
    Client client(*it);

    // Update the 'allocations' to reflect the allocator decision.
    client.allocations++;

    // Remove and reinsert it to update the ordering appropriately.
    clients.erase(it);
    clients.insert(client);
  }

  // Add shared resources to the allocated quantities when the same
  // resources don't already exist in the allocation.
  const Resources newShared = resources.shared()
    .filter([this, name, slaveId](const Resource& resource) {
      return !allocations[name].resources[slaveId].contains(resource);
    });

  const Resources scalarQuantities =
    (resources.nonShared() + newShared).createStrippedScalarQuantity();

  allocations[name].resources[slaveId] += resources;
  allocations[name].scalarQuantities += scalarQuantities;

  foreach (const Resource& resource, scalarQuantities) {
    allocations[name].totals[resource.name()] += resource.scalar();
  }

  // If the total resources have changed, we're going to recalculate
  // all the shares, so don't bother just updating this client.
  if (!dirty) {
    updateShare(name);
  }
}


void DRFSorter::update(
    const string& name,
    const SlaveID& slaveId,
    const Resources& oldAllocation,
    const Resources& newAllocation)
{
  CHECK(contains(name));

  // TODO(bmahler): Check invariants between old and new allocations.
  // Namely, the roles and quantities of resources should be the same!
  // Otherwise, we need to ensure we re-calculate the shares, as
  // is being currently done, for safety.

  const Resources oldAllocationQuantity =
    oldAllocation.createStrippedScalarQuantity();
  const Resources newAllocationQuantity =
    newAllocation.createStrippedScalarQuantity();

  CHECK(allocations[name].resources[slaveId].contains(oldAllocation));
  CHECK(allocations[name].scalarQuantities.contains(oldAllocationQuantity));

  allocations[name].resources[slaveId] -= oldAllocation;
  allocations[name].resources[slaveId] += newAllocation;

  allocations[name].scalarQuantities -= oldAllocationQuantity;
  allocations[name].scalarQuantities += newAllocationQuantity;

  foreach (const Resource& resource, oldAllocationQuantity) {
    allocations[name].totals[resource.name()] -= resource.scalar();
  }

  foreach (const Resource& resource, newAllocationQuantity) {
    allocations[name].totals[resource.name()] += resource.scalar();
  }

  // Just assume the total has changed, per the TODO above.
  dirty = true;
}


const hashmap<SlaveID, Resources>& DRFSorter::allocation(
    const string& name) const
{
  CHECK(contains(name));

  return allocations.at(name).resources;
}


const Resources& DRFSorter::allocationScalarQuantities(
    const string& name) const
{
  CHECK(contains(name));

  return allocations.at(name).scalarQuantities;
}


hashmap<string, Resources> DRFSorter::allocation(const SlaveID& slaveId) const
{
  // TODO(jmlvanre): We can index the allocation by slaveId to make this faster.
  // It is a tradeoff between speed vs. memory. For now we use existing data
  // structures.

  hashmap<string, Resources> result;

  foreachpair (const string& name, const Allocation& allocation, allocations) {
    if (allocation.resources.contains(slaveId)) {
      // It is safe to use `at()` here because we've just checked the existence
      // of the key. This avoid un-necessary copies.
      result.emplace(name, allocation.resources.at(slaveId));
    }
  }

  return result;
}


Resources DRFSorter::allocation(
    const string& name,
    const SlaveID& slaveId) const
{
  CHECK(contains(name));

  if (allocations.at(name).resources.contains(slaveId)) {
    return allocations.at(name).resources.at(slaveId);
  }

  return Resources();
}


const Resources& DRFSorter::totalScalarQuantities() const
{
  return total_.scalarQuantities;
}


void DRFSorter::unallocated(
    const string& name,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(contains(name));
  CHECK(allocations.at(name).resources.contains(slaveId));
  CHECK(allocations.at(name).resources.at(slaveId).contains(resources));

  allocations[name].resources[slaveId] -= resources;

  // Remove shared resources from the allocated quantities when there
  // are no instances of same resources left in the allocation.
  const Resources absentShared = resources.shared()
    .filter([this, name, slaveId](const Resource& resource) {
      return !allocations[name].resources[slaveId].contains(resource);
    });

  const Resources scalarQuantities =
    (resources.nonShared() + absentShared).createStrippedScalarQuantity();

  foreach (const Resource& resource, scalarQuantities) {
    allocations[name].totals[resource.name()] -= resource.scalar();
  }

  CHECK(allocations[name].scalarQuantities.contains(scalarQuantities));
  allocations[name].scalarQuantities -= scalarQuantities;

  if (allocations[name].resources[slaveId].empty()) {
    allocations[name].resources.erase(slaveId);
  }

  if (!dirty) {
    updateShare(name);
  }
}


void DRFSorter::add(const SlaveID& slaveId, const Resources& resources)
{
  if (!resources.empty()) {
    // Add shared resources to the total quantities when the same
    // resources don't already exist in the total.
    const Resources newShared = resources.shared()
      .filter([this, slaveId](const Resource& resource) {
        return !total_.resources[slaveId].contains(resource);
      });

    total_.resources[slaveId] += resources;

    const Resources scalarQuantities =
      (resources.nonShared() + newShared).createStrippedScalarQuantity();

    total_.scalarQuantities += scalarQuantities;

    foreach (const Resource& resource, scalarQuantities) {
      total_.totals[resource.name()] += resource.scalar();
    }

    // We have to recalculate all shares when the total resources
    // change, but we put it off until sort is called so that if
    // something else changes before the next allocation we don't
    // recalculate everything twice.
    dirty = true;
  }
}


void DRFSorter::remove(const SlaveID& slaveId, const Resources& resources)
{
  if (!resources.empty()) {
    CHECK(total_.resources.contains(slaveId));
    CHECK(total_.resources[slaveId].contains(resources))
      << total_.resources[slaveId] << " does not contain " << resources;

    total_.resources[slaveId] -= resources;

    // Remove shared resources from the total quantities when there
    // are no instances of same resources left in the total.
    const Resources absentShared = resources.shared()
      .filter([this, slaveId](const Resource& resource) {
        return !total_.resources[slaveId].contains(resource);
      });

    const Resources scalarQuantities =
      (resources.nonShared() + absentShared).createStrippedScalarQuantity();

    foreach (const Resource& resource, scalarQuantities) {
      total_.totals[resource.name()] -= resource.scalar();
    }

    CHECK(total_.scalarQuantities.contains(scalarQuantities));
    total_.scalarQuantities -= scalarQuantities;

    if (total_.resources[slaveId].empty()) {
      total_.resources.erase(slaveId);
    }

    dirty = true;
  }
}


vector<string> DRFSorter::sort()
{
  if (dirty) {
    set<Client, DRFComparator> temp;

    foreach (Client client, clients) {
      // Update the 'share' to get proper sorting.
      client.share = calculateShare(client.name);

      temp.insert(client);
    }

    clients = temp;

    // Reset dirty to false so as not to re-calculate *all*
    // shares unless another dirtying operation occurs.
    dirty = false;
  }

  vector<string> result;
  result.reserve(clients.size());

  foreach (const Client& client, clients) {
    result.push_back(client.name);
  }

  return result;
}


bool DRFSorter::contains(const string& name) const
{
  return allocations.contains(name);
}


int DRFSorter::count() const
{
  return allocations.size();
}


void DRFSorter::updateShare(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) {
    Client client(*it);

    // Update the 'share' to get proper sorting.
    client.share = calculateShare(client.name);

    // Remove and reinsert it to update the ordering appropriately.
    clients.erase(it);
    clients.insert(client);
  }
}


double DRFSorter::calculateShare(const string& name) const
{
  CHECK(contains(name));

  double share = 0.0;

  // TODO(benh): This implementation of "dominant resource fairness"
  // currently does not take into account resources that are not
  // scalars.

  foreachpair (const string& resourceName,
               const Value::Scalar& scalar,
               total_.totals) {
    // Filter out the resources excluded from fair sharing.
    if (fairnessExcludeResourceNames.isSome() &&
        fairnessExcludeResourceNames->count(resourceName) > 0) {
      continue;
    }

    if (scalar.value() > 0.0 &&
        allocations.at(name).totals.contains(resourceName)) {
      const double allocation =
        allocations.at(name).totals.at(resourceName).value();

      share = std::max(share, allocation / scalar.value());
    }
  }

  return share / weights.at(name);
}


set<Client, DRFComparator>::iterator DRFSorter::find(const string& name)
{
  set<Client, DRFComparator>::iterator it;
  for (it = clients.begin(); it != clients.end(); it++) {
    if (name == it->name) {
      break;
    }
  }

  return it;
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
