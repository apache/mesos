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

#ifndef __MASTER_ALLOCATOR_ALLOCATOR_HPP__
#define __MASTER_ALLOCATOR_ALLOCATOR_HPP__

#include <string>
#include <vector>

#include <mesos/resources.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"
#include "messages/messages.hpp"

namespace mesos {
namespace internal {
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
  Allocator() {}

  virtual ~Allocator() {}

  virtual void initialize(
      const Flags& flags,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>& offerCallback,
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

  // Note that the 'total' resources are passed explicitly because it
  // includes resources that are dynamically "checkpointed" on the
  // slave (e.g. persistent volumes, dynamic reservations, etc). The
  // slaveInfo resources, on the other hand, correspond directly to
  // the static --resources flag value on the slave.
  virtual void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void removeSlave(
      const SlaveID& slaveId) = 0;

  // Offers are sent only for activated slaves.
  virtual void activateSlave(
      const SlaveID& slaveId) = 0;

  virtual void deactivateSlave(
      const SlaveID& slaveId) = 0;

  virtual void updateWhitelist(
      const Option<hashset<std::string> >& whitelist) = 0;

  virtual void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests) = 0;

  virtual void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations) = 0;

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
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_ALLOCATOR_HPP__
