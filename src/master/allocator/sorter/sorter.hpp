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

#ifndef __MASTER_ALLOCATOR_SORTER_SORTER_HPP__
#define __MASTER_ALLOCATOR_SORTER_SORTER_HPP__

#include <functional>
#include <string>
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

  // Adds a client to allocate resources to. A client
  // may be a user or a framework.
  virtual void add(const std::string& client, double weight = 1) = 0;

  // Updates the weight of a client. The client must have previously
  // be added to the sorter, but it may currently be inactive.
  virtual void update(const std::string& client, double weight) = 0;

  // Removes a client.
  virtual void remove(const std::string& client) = 0;

  // Readds a client to the sort after deactivate.
  // It is a no-op if the client is already in the sort.
  virtual void activate(const std::string& client) = 0;

  // Removes a client from the sort, so it won't get allocated to.
  // It is a no-op if the client is already not in the sort.
  virtual void deactivate(const std::string& client) = 0;

  // Specify that resources have been allocated to the given client.
  virtual void allocated(
      const std::string& client,
      const SlaveID& slaveId,
      const Resources& resources) = 0;

  // Updates a portion of the allocation for the client, in order to
  // augment the resources with additional metadata (e.g., volumes).
  // This means that the new allocation must not affect the static
  // roles, or the overall quantities of resources!
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
  // this client. This omits metadata about dynamic reservations and
  // persistent volumes; see `Resources::createStrippedScalarQuantity`.
  virtual const Resources& allocationScalarQuantities(
      const std::string& client) const = 0;

  // Returns the clients that have allocations on this slave.
  virtual hashmap<std::string, Resources> allocation(
      const SlaveID& slaveId) const = 0;

  // Returns the given slave's resources that have been allocated to
  // this client.
  virtual Resources allocation(
      const std::string& client,
      const SlaveID& slaveId) const = 0;

  // Returns the total scalar resource quantities in this sorter. This
  // omits metadata about dynamic reservations and persistent volumes; see
  // `Resources::createStrippedScalarQuantity`.
  virtual const Resources& totalScalarQuantities() const = 0;

  // Add resources to the total pool of resources this
  // Sorter should consider.
  virtual void add(const SlaveID& slaveId, const Resources& resources) = 0;

  // Remove resources from the total pool.
  virtual void remove(const SlaveID& slaveId, const Resources& resources) = 0;

  // Returns all of the clients in the order that they should
  // be allocated to, according to this Sorter's policy.
  virtual std::vector<std::string> sort() = 0;

  // Returns true if this Sorter contains the specified client,
  // which may be active or inactive.
  virtual bool contains(const std::string& client) const = 0;

  // Returns the number of clients this Sorter contains,
  // either active or inactive.
  virtual int count() const = 0;
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_SORTER_SORTER_HPP__
