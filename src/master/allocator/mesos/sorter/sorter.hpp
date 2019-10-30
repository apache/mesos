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

#ifndef __MASTER_ALLOCATOR_MESOS_SORTER_SORTER_HPP__
#define __MASTER_ALLOCATOR_MESOS_SORTER_SORTER_HPP__

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/pid.hpp>

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// Sorters implement the logic for determining the
// order in which users or frameworks should receive
// resource allocations.
//
// TODO(bmahler): Templatize this on Client, so that callers can
// don't need to do string conversion, e.g. FrameworkID, string role,
// etc.
class Sorter
{
public:
  Sorter() = default;

  // Provides the allocator's execution context (via a UPID)
  // and a name prefix in order to support metrics within the
  // sorter implementation.
  explicit Sorter(
      const process::UPID& allocator,
      const std::string& metricsPrefix) {}

  virtual ~Sorter() = default;

  // Initialize the sorter.
  virtual void initialize(
      const Option<std::set<std::string>>& fairnessExcludeResourceNames) = 0;

  // Adds a client to allocate resources to.
  // A client may be a user or a framework.
  // This function will not activate the client.
  virtual void add(const std::string& client) = 0;

  // Removes a client.
  virtual void remove(const std::string& client) = 0;

  // Readds a client to the sort after deactivate.
  // It is a no-op if the client is already in the sort.
  virtual void activate(const std::string& client) = 0;

  // Removes a client from the sort, so it won't get allocated to.
  // It is a no-op if the client is already not in the sort.
  virtual void deactivate(const std::string& client) = 0;

  // Updates the weight of a client path. This changes the sorter's
  // behavior for all clients in the subtree identified by this path
  // (both clients currently in the sorter and any clients that may be
  // added later). If a client's weight is not explicitly set, the
  // default weight of 1.0 is used. This interface does not support
  // unsetting previously set weights; instead, the weight should be
  // reset to the default value.
  virtual void updateWeight(const std::string& path, double weight) = 0;

  // Specify that resources have been allocated to the given client.
  virtual void allocated(
      const std::string& client,
      const SlaveID& slaveId,
      const Resources& resources) = 0;

  // Updates a portion of the allocation for the client, in order to augment the
  // resources with additional metadata (e.g., volumes), or remove certain
  // resources. If the roles or scalar quantities are changed, the order of the
  // clients should be updated accordingly.
  virtual void update(
      const std::string& client,
      const SlaveID& slaveId,
      const Resources& oldAllocation,
      const Resources& newAllocation) = 0;

  // Specify that resources have been unallocated from the given client.
  virtual void unallocated(
      const std::string& client,
      const SlaveID& slaveId,
      const Resources& resources) = 0;

  // Returns the resources that have been allocated to this client.
  virtual const hashmap<SlaveID, Resources>& allocation(
      const std::string& client) const = 0;

  // Returns the total scalar resource quantities that are allocated to
  // a client, or all clients if a client is not provided.
  virtual const ResourceQuantities& allocationScalarQuantities(
      const std::string& client) const = 0;
  virtual const ResourceQuantities& allocationScalarQuantities() const = 0;

  // Returns the given slave's resources that have been allocated to
  // this client.
  virtual Resources allocation(
      const std::string& client,
      const SlaveID& slaveId) const = 0;

  // Add/remove total scalar resource quantities of an agent to/from the
  // total pool of resources this Sorter should consider.
  //
  // NOTE: Updating resources of an agent in the Sorter is done by first calling
  // `removeSlave()` and then `addSlave()` with new resource quantities.
  //
  // NOTE: Attempt to add the same agent twice or remove an agent not added
  // to the Sorter may crash the program.
  virtual void addSlave(
      const SlaveID& slaveId,
      const ResourceQuantities& scalarQuantities) = 0;

  virtual void removeSlave(const SlaveID& slaveId) = 0;

  // Returns all of the clients in the order that they should
  // be allocated to, according to this Sorter's policy.
  virtual std::vector<std::string> sort() = 0;

  // Returns true if this Sorter contains the specified client,
  // which may be active or inactive.
  virtual bool contains(const std::string& client) const = 0;

  // Returns the number of clients this Sorter contains,
  // either active or inactive.
  virtual size_t count() const = 0;
};


} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_SORTER_SORTER_HPP__
