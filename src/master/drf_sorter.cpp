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

#include "logging/logging.hpp"

#include "master/drf_sorter.hpp"

using std::list;
using std::set;
using std::string;


namespace mesos {
namespace internal {
namespace master {
namespace allocator {

bool DRFComparator::operator () (const Client& client1, const Client& client2)
{
  if (client1.share == client2.share) {
    if (client1.allocations == client2.allocations) {
      return client1.name < client2.name;
    }
    return client1.allocations < client2.allocations;
  }
  return client1.share < client2.share;
}


void DRFSorter::add(const string& name, double weight)
{
  Client client(name, 0, 0);
  clients.insert(client);

  allocations[name] = Resources();
  weights[name] = weight;
}


void DRFSorter::remove(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) {
    clients.erase(it);
  }

  allocations.erase(name);
  weights.erase(name);
}


void DRFSorter::activate(const string& name)
{
  CHECK(allocations.contains(name));

  Client client(name, calculateShare(name), 0);
  clients.insert(client);
}


void DRFSorter::deactivate(const string& name)
{
  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) {
    // TODO(benh): Removing the client is an unfortuante strategy
    // because we lose information such as the number of allocations
    // for this client which means the fairness can be gamed by a
    // framework disconnecting and reconnecting.
    clients.erase(it);
  }
}


void DRFSorter::allocated(
    const string& name,
    const Resources& resources)
{
  set<Client, DRFComparator>::iterator it = find(name);

  if (it != clients.end()) { // TODO(benh): This should really be a CHECK.
    // TODO(benh): Refactor 'update' to be able to reuse it here.
    Client client(*it);

    // Update the 'allocations' to reflect the allocator decision.
    client.allocations++;

    // Remove and reinsert it to update the ordering appropriately.
    clients.erase(it);
    clients.insert(client);
  }

  allocations[name] += resources;

  // If the total resources have changed, we're going to
  // recalculate all the shares, so don't bother just
  // updating this client.
  if (!dirty) {
    update(name);
  }
}


void DRFSorter::update(
    const string& name,
    const Resources& oldAllocation,
    const Resources& newAllocation)
{
  CHECK(contains(name));

  // TODO(bmahler): Check invariants between old and new allocations.
  // Namely, the roles and quantities of resources should be the same!
  // Otherwise, we need to ensure we re-calculate the shares, as
  // is being currently done, for safety.

  CHECK(resources.contains(oldAllocation));

  resources -= oldAllocation;
  resources += newAllocation;

  CHECK(allocations[name].contains(oldAllocation));

  allocations[name] -= oldAllocation;
  allocations[name] += newAllocation;

  // Just assume the total has changed, per the TODO above.
  dirty = true;
}


Resources DRFSorter::allocation(
    const string& name)
{
  return allocations[name];
}


void DRFSorter::unallocated(
    const string& name,
    const Resources& resources)
{
  allocations[name] -= resources;

  if (!dirty) {
    update(name);
  }
}


void DRFSorter::add(const Resources& _resources)
{
  resources += _resources;

  // We have to recalculate all shares when the total resources
  // change, but we put it off until sort is called
  // so that if something else changes before the next allocation
  // we don't recalculate everything twice.
  dirty = true;
}


void DRFSorter::remove(const Resources& _resources)
{
  resources -= _resources;
  dirty = true;
}


list<string> DRFSorter::sort()
{
  if (dirty) {
    set<Client, DRFComparator> temp;

    set<Client, DRFComparator>::iterator it;
    for (it = clients.begin(); it != clients.end(); it++) {
      Client client(*it);

      // Update the 'share' to get proper sorting.
      client.share = calculateShare(client.name);

      temp.insert(client);
    }

    clients = temp;
  }

  list<string> result;

  set<Client, DRFComparator>::iterator it;
  for (it = clients.begin(); it != clients.end(); it++) {
    result.push_back((*it).name);
  }

  return result;
}


bool DRFSorter::contains(const string& name)
{
  return allocations.contains(name);
}


int DRFSorter::count()
{
  return allocations.size();
}


void DRFSorter::update(const string& name)
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


double DRFSorter::calculateShare(const string& name)
{
  double share = 0;

  // TODO(benh): This implementation of "dominant resource fairness"
  // currently does not take into account resources that are not
  // scalars.

  // Scalar resources may be spread across multiple 'Resource'
  // objects. E.g. persistent volumes. So we first collect the names
  // of the scalar resources, before computing the totals.
  hashset<string> scalars;
  foreach (const Resource& resource, resources) {
    if (resource.type() == Value::SCALAR) {
      scalars.insert(resource.name());
    }
  }

  foreach (const string& scalar, scalars) {
    Option<Value::Scalar> total = resources.get<Value::Scalar>(scalar);

    if (total.isSome() && total.get().value() > 0) {
      Option<Value::Scalar> allocation =
        allocations[name].get<Value::Scalar>(scalar);

      if (allocation.isNone()) {
        allocation = Value::Scalar();
      }

      share = std::max(share, allocation.get().value() / total.get().value());
    }
  }

  return share / weights[name];
}


set<Client, DRFComparator>::iterator DRFSorter::find(const string& name)
{
  set<Client, DRFComparator>::iterator it;
  for (it = clients.begin(); it != clients.end(); it++) {
    if (name == (*it).name) {
      break;
    }
  }

  return it;
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
