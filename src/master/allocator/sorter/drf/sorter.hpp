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

#ifndef __MASTER_ALLOCATOR_SORTER_DRF_SORTER_HPP__
#define __MASTER_ALLOCATOR_SORTER_DRF_SORTER_HPP__

#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "master/allocator/sorter/drf/metrics.hpp"

#include "master/allocator/sorter/sorter.hpp"


namespace mesos {
namespace internal {
namespace master {
namespace allocator {

struct Client
{
  Client(const std::string& _name, double _share, uint64_t _allocations)
    : name(_name), share(_share), allocations(_allocations) {}

  std::string name;
  double share;

  // We store the number of times this client has been chosen for
  // allocation so that we can fairly share the resources across
  // clients that have the same share. Note that this information is
  // not persisted across master failovers, but since the point is to
  // equalize the 'allocations' across clients of the same 'share'
  // having allocations restart at 0 after a master failover should be
  // sufficient (famous last words.)
  uint64_t allocations;
};


struct DRFComparator
{
  virtual ~DRFComparator() {}
  virtual bool operator()(const Client& client1, const Client& client2);
};


class DRFSorter : public Sorter
{
public:
  DRFSorter() = default;

  explicit DRFSorter(
      const process::UPID& allocator,
      const std::string& metricsPrefix);

  virtual ~DRFSorter() {}

  virtual void initialize(
      const Option<std::set<std::string>>& fairnessExcludeResourceNames);

  virtual void add(const std::string& name, double weight = 1);

  virtual void update(const std::string& name, double weight);

  virtual void remove(const std::string& name);

  virtual void activate(const std::string& name);

  virtual void deactivate(const std::string& name);

  virtual void allocated(
      const std::string& name,
      const SlaveID& slaveId,
      const Resources& resources);

  virtual void update(
      const std::string& name,
      const SlaveID& slaveId,
      const Resources& oldAllocation,
      const Resources& newAllocation);

  virtual void unallocated(
      const std::string& name,
      const SlaveID& slaveId,
      const Resources& resources);

  virtual const hashmap<SlaveID, Resources>& allocation(
      const std::string& name);

  virtual const Resources& allocationScalarQuantities(const std::string& name);

  virtual hashmap<std::string, Resources> allocation(const SlaveID& slaveId);

  virtual Resources allocation(const std::string& name, const SlaveID& slaveId);

  virtual const Resources& totalScalarQuantities() const;

  virtual void add(const SlaveID& slaveId, const Resources& resources);

  virtual void remove(const SlaveID& slaveId, const Resources& resources);

  virtual std::vector<std::string> sort();

  virtual bool contains(const std::string& name) const;

  virtual int count();

private:
  // Recalculates the share for the client and moves
  // it in 'clients' accordingly.
  void update(const std::string& name);

  // Returns the dominant resource share for the client.
  double calculateShare(const std::string& name) const;

  // Resources (by name) that will be excluded from fair sharing.
  Option<std::set<std::string>> fairnessExcludeResourceNames;

  // Returns an iterator to the specified client, if
  // it exists in this Sorter.
  std::set<Client, DRFComparator>::iterator find(const std::string& name);

  // If true, sort() will recalculate all shares.
  bool dirty = false;

  // A set of Clients (names and shares) sorted by share.
  std::set<Client, DRFComparator> clients;

  // Maps client names to the weights that should be applied to their shares.
  hashmap<std::string, double> weights;

  // Total resources.
  struct Total {
    // We need to keep track of the resources (and not just scalar quantities)
    // to account for multiple copies of the same shared resources. We need to
    // ensure that we do not update the scalar quantities for shared resources
    // when the change is only in the number of copies in the sorter.
    hashmap<SlaveID, Resources> resources;

    // NOTE: Scalars can be safely aggregated across slaves. We keep
    // that to speed up the calculation of shares. See MESOS-2891 for
    // the reasons why we want to do that.
    //
    // NOTE: We omit information about dynamic reservations and persistent
    // volumes here to enable resources to be aggregated across slaves
    // more effectively. See MESOS-4833 for more information.
    //
    // Sharedness info is also stripped out when resource identities are
    // omitted because sharedness inherently refers to the identities of
    // resources and not quantities.
    Resources scalarQuantities;

    // We also store a map version of `scalarQuantities`, mapping
    // the `Resource::name` to aggregated scalar. This improves the
    // performance of calculating shares. See MESOS-4694.
    //
    // TODO(bmahler): Ideally we do not store `scalarQuantities`
    // redundantly here, investigate performance improvements to
    // `Resources` to make this unnecessary.
    hashmap<std::string, Value::Scalar> totals;
  } total_;

  // Allocation for a client.
  struct Allocation {
    // We maintain multiple copies of each shared resource allocated
    // to a client, where the number of copies represents the number
    // of times this shared resource has been allocated to (and has
    // not been recovered from) a specific client.
    hashmap<SlaveID, Resources> resources;

    // Similarly, we aggregate scalars across slaves and omit information
    // about dynamic reservations, persistent volumes and sharedness of
    // the corresponding resource. See notes above.
    Resources scalarQuantities;

    // We also store a map version of `scalarQuantities`, mapping
    // the `Resource::name` to aggregated scalar. This improves the
    // performance of calculating shares. See MESOS-4694.
    //
    // TODO(bmahler): Ideally we do not store `scalarQuantities`
    // redundantly here, investigate performance improvements to
    // `Resources` to make this unnecessary.
    hashmap<std::string, Value::Scalar> totals;
  };

  // Maps client names to the resources they have been allocated.
  hashmap<std::string, Allocation> allocations;

  // Metrics are optionally exposed by the sorter.
  friend Metrics;
  Option<Metrics> metrics;
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_SORTER_DRF_SORTER_HPP__
