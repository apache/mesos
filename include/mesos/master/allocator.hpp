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

#ifndef __MESOS_MASTER_ALLOCATOR_HPP__
#define __MESOS_MASTER_ALLOCATOR_HPP__

#include <string>
#include <vector>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/master/allocator.pb.h>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace master {
namespace allocator {

// Basic model of an allocator: resources are allocated to a framework
// in the form of offers. A framework can refuse some resources in
// offers and run tasks in others. Allocated resources can have offer
// operations applied to them in order for frameworks to alter the
// resource metadata (e.g. creating persistent volumes). Resources can
// be recovered from a framework when tasks finish/fail (or are lost
// due to a slave failure) or when an offer is rescinded.
//
// This is the public API for resource allocators.
// TODO(alexr): Document API calls.
class Allocator
{
public:
  // Attempts either to create a built-in DRF allocator or to load an
  // allocator instance from a module using the given name. If Try
  // does not report an error, the wrapped Allocator* is not null.
  static Try<Allocator*> create(const std::string& name);

  Allocator() {}

  virtual ~Allocator() {}

  virtual void initialize(
      const Duration& allocationInterval,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>& offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback,
      const hashmap<std::string, RoleInfo>& roles) = 0;

  virtual void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used) = 0;

  virtual void removeFramework(
      const FrameworkID& frameworkId) = 0;

  // Offers are sent only to activated frameworks.
  virtual void activateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void deactivateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo) = 0;

  // Note that the 'total' resources are passed explicitly because it
  // includes resources that are dynamically "checkpointed" on the
  // slave (e.g. persistent volumes, dynamic reservations, etc). The
  // slaveInfo resources, on the other hand, correspond directly to
  // the static --resources flag value on the slave.
  virtual void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void removeSlave(
      const SlaveID& slaveId) = 0;

  // Note that 'oversubscribed' resources include the total amount of
  // oversubscribed resources that are allocated and available.
  // TODO(vinod): Instead of just oversubscribed resources have this
  // method take total resources. We can then reuse this method to
  // update slave's total resources in the future.
  virtual void updateSlave(
      const SlaveID& slave,
      const Resources& oversubscribed) = 0;

  // Offers are sent only for activated slaves.
  virtual void activateSlave(
      const SlaveID& slaveId) = 0;

  virtual void deactivateSlave(
      const SlaveID& slaveId) = 0;

  virtual void updateWhitelist(
      const Option<hashset<std::string>>& whitelist) = 0;

  virtual void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests) = 0;

  virtual void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations) = 0;

  virtual process::Future<Nothing> updateAvailable(
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations) = 0;

  // We currently support storing the next unavailability, if there is one, per
  // slave. If `unavailability` is not set then there is no known upcoming
  // unavailability. This might require the implementation of the function to
  // remove any inverse offers that are outstanding.
  virtual void updateUnavailability(
      const SlaveID& slaveId,
      const Option<Unavailability>& unavailability) = 0;

  // Informs the allocator that the inverse offer has been responded to or
  // revoked. If `status` is not set then the inverse offer was not responded
  // to, possibly because the offer timed out or was rescinded. This might
  // require the implementation of the function to remove any inverse offers
  // that are outstanding. The `unavailableResources` can be used by the
  // allocator to distinguish between different inverse offers sent to the same
  // framework for the same slave.
  virtual void updateInverseOffer(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const Option<UnavailableResources>& unavailableResources,
      const Option<InverseOfferStatus>& status,
      const Option<Filters>& filters = None()) = 0;

  // Retrieves the status of all inverse offers maintained by the allocator.
  virtual process::Future<
      hashmap<SlaveID, hashmap<FrameworkID, mesos::master::InverseOfferStatus>>>
    getInverseOfferStatuses() = 0;

  // Informs the Allocator to recover resources that are considered
  // used by the framework.
  virtual void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters) = 0;

  // Whenever a framework that has filtered resources wants to revive
  // offers for those resources the master invokes this callback.
  virtual void reviveOffers(
      const FrameworkID& frameworkId) = 0;

  // Informs the allocator to stop sending resources for the framework
  virtual void suppressOffers(
      const FrameworkID& frameworkId) = 0;
};

} // namespace allocator {
} // namespace master {
} // namespace mesos {

#endif // __MESOS_MASTER_ALLOCATOR_HPP__
