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
  if (client1.share != client2.share) {
    return client1.share < client2.share;
  }

  if (client1.allocation.count != client2.allocation.count) {
    return client1.allocation.count < client2.allocation.count;
  }

  return client1.name < client2.name;
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


void DRFSorter::add(const string& name)
{
  CHECK(!contains(name));

  Client client(name);
  clients.insert(client);

  if (metrics.isSome()) {
    metrics->add(name);
  }
}


void DRFSorter::remove(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  clients.erase(it);

  if (metrics.isSome()) {
    metrics->remove(name);
  }
}


void DRFSorter::activate(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  if (!it->active) {
    Client client(*it);
    client.active = true;

    clients.erase(it);
    clients.insert(client);
  }
}


void DRFSorter::deactivate(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  if (it->active) {
    Client client(*it);
    client.active = false;

    clients.erase(it);
    clients.insert(client);
  }
}


void DRFSorter::updateWeight(const string& name, double weight)
{
  weights[name] = weight;

  // It would be possible to avoid dirtying the tree here (in some
  // cases), but it doesn't seem worth the complexity.
  dirty = true;
}


void DRFSorter::allocated(
    const string& name,
    const SlaveID& slaveId,
    const Resources& resources)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  Client client(*it);
  client.allocation.add(slaveId, resources);

  clients.erase(it);
  clients.insert(client);

  // If the total resources have changed, we're going to recalculate
  // all the shares, so don't bother just updating this client.
  if (!dirty) {
    updateShare(client.name);
  }
}


void DRFSorter::update(
    const string& name,
    const SlaveID& slaveId,
    const Resources& oldAllocation,
    const Resources& newAllocation)
{
  // TODO(bmahler): Check invariants between old and new allocations.
  // Namely, the roles and quantities of resources should be the same!
  // Otherwise, we need to ensure we re-calculate the shares, as
  // is being currently done, for safety.

  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  Client client(*it);
  client.allocation.update(slaveId, oldAllocation, newAllocation);

  clients.erase(it);
  clients.insert(client);

  // Just assume the total has changed, per the TODO above.
  dirty = true;
}


void DRFSorter::unallocated(
    const string& name,
    const SlaveID& slaveId,
    const Resources& resources)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  Client client(*it);
  client.allocation.subtract(slaveId, resources);

  clients.erase(it);
  clients.insert(client);

  // If the total resources have changed, we're going to recalculate
  // all the shares, so don't bother just updating this client.
  if (!dirty) {
    updateShare(client.name);
  }
}


const hashmap<SlaveID, Resources>& DRFSorter::allocation(
    const string& name) const
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  return it->allocation.resources;
}


const Resources& DRFSorter::allocationScalarQuantities(
    const string& name) const
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  return it->allocation.scalarQuantities;
}


hashmap<string, Resources> DRFSorter::allocation(const SlaveID& slaveId) const
{
  // TODO(jmlvanre): We can index the allocation by slaveId to make this faster.
  // It is a tradeoff between speed vs. memory. For now we use existing data
  // structures.

  hashmap<string, Resources> result;

  foreach (const Client& client, clients) {
    if (client.allocation.resources.contains(slaveId)) {
      // It is safe to use `at()` here because we've just checked the
      // existence of the key. This avoid un-necessary copies.
      result.emplace(client.name, client.allocation.resources.at(slaveId));
    }
  }

  return result;
}


Resources DRFSorter::allocation(
    const string& name,
    const SlaveID& slaveId) const
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  if (it->allocation.resources.contains(slaveId)) {
    return it->allocation.resources.at(slaveId);
  }

  return Resources();
}


const Resources& DRFSorter::totalScalarQuantities() const
{
  return total_.scalarQuantities;
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
      client.share = calculateShare(client);

      temp.insert(client);
    }

    clients = temp;

    // Reset dirty to false so as not to re-calculate *all*
    // shares unless another dirtying operation occurs.
    dirty = false;
  }

  vector<string> result;

  foreach (const Client& client, clients) {
    if (client.active) {
      result.push_back(client.name);
    }
  }

  return result;
}


bool DRFSorter::contains(const string& name) const
{
  set<Client, DRFComparator>::iterator it = find(name);
  return it != clients.end();
}


int DRFSorter::count() const
{
  return clients.size();
}


void DRFSorter::updateShare(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);
  CHECK(it != clients.end());

  Client client(*it);

  // Update the 'share' to get proper sorting.
  client.share = calculateShare(client);

  // Remove and reinsert it to update the ordering appropriately.
  clients.erase(it);
  clients.insert(client);
}


double DRFSorter::calculateShare(const Client& client) const
{
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
        client.allocation.totals.contains(resourceName)) {
      const double allocation =
        client.allocation.totals.at(resourceName).value();

      share = std::max(share, allocation / scalar.value());
    }
  }

  return share / clientWeight(client.name);
}


double DRFSorter::clientWeight(const string& name) const
{
  Option<double> weight = weights.get(name);

  if (weight.isNone()) {
    return 1.0;
  }

  return weight.get();
}


set<Client, DRFComparator>::iterator DRFSorter::find(const string& name) const
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
