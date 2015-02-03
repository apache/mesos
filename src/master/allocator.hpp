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

#ifndef __ALLOCATOR_HPP__
#define __ALLOCATOR_HPP__

#include <string>
#include <vector>

#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/dispatch.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace master {

class Master; // Forward declaration.

namespace allocator {

class AllocatorProcess; // Forward declaration.

// Basic model of an allocator: resources are allocated to a framework
// in the form of offers. A framework can refuse some resources in
// offers and run tasks in others. Allocated resources can have offer
// operations applied to them in order for frameworks to alter the
// resource metadata (e.g. creating persistent volumes). Resources can
// be recovered from a framework when tasks finish/fail (or are lost
// due to a slave failure) or when an offer is rescinded.
//
// NOTE: DO NOT subclass this class when implementing a new allocator.
// Implement AllocatorProcess (above) instead!
class Allocator
{
public:
  // The AllocatorProcess object passed to the constructor is spawned
  // and terminated by the allocator. But it is the responsibility
  // of the caller to de-allocate the object, if necessary.
  explicit Allocator(AllocatorProcess* _process);

  virtual ~Allocator();

  void initialize(
      const Flags& flags,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>& offerCallback,
      const hashmap<std::string, RoleInfo>& roles);

  void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used);

  void removeFramework(
      const FrameworkID& frameworkId);

  // Offers are sent only to activated frameworks.
  void activateFramework(
      const FrameworkID& frameworkId);

  void deactivateFramework(
      const FrameworkID& frameworkId);

  // Note that the 'total' resources are passed explicitly because it
  // includes resources that are dynamically "checkpointed" on the
  // slave (e.g. persistent volumes, dynamic reservations, etc). The
  // slaveInfo resources, on the other hand, correspond directly to
  // the static --resources flag value on the slave.
  void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used);

  void removeSlave(
      const SlaveID& slaveId);

  // Offers are sent only for activated slaves.
  void activateSlave(
      const SlaveID& slaveId);

  void deactivateSlave(
      const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string> >& whitelist);

  void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations);

  // Informs the allocator to recover resources that are considered
  // used by the framework.
  void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  // Whenever a framework that has filtered resources wants to revive
  // offers for those resources the master invokes this callback.
  void reviveOffers(
      const FrameworkID& frameworkId);

private:
  Allocator(const Allocator&); // Not copyable.
  Allocator& operator=(const Allocator&); // Not assignable.

  AllocatorProcess* process;
};


class AllocatorProcess : public process::Process<AllocatorProcess>
{
public:
  AllocatorProcess() {}

  virtual ~AllocatorProcess() {}

  // Explicitly unhide 'initialize' to silence a compiler warning
  // from clang, since we overload below.
  using process::ProcessBase::initialize;

  virtual void initialize(
      const Flags& flags,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>& offerCallback,
      const hashmap<std::string, RoleInfo>& roles) = 0;

  virtual void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used) = 0;

  virtual void removeFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void activateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void deactivateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void removeSlave(
      const SlaveID& slaveId) = 0;

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

  virtual void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters) = 0;

  virtual void reviveOffers(
      const FrameworkID& frameworkId) = 0;
};


inline Allocator::Allocator(AllocatorProcess* _process)
  : process(_process)
{
  process::spawn(process);
}


inline Allocator::~Allocator()
{
  process::terminate(process);
  process::wait(process);
}


inline void Allocator::initialize(
    const Flags& flags,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, Resources>&)>& offerCallback,
    const hashmap<std::string, RoleInfo>& roles)
{
  process::dispatch(
      process,
      &AllocatorProcess::initialize,
      flags,
      offerCallback,
      roles);
}


inline void Allocator::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  process::dispatch(
      process,
      &AllocatorProcess::addFramework,
      frameworkId,
      frameworkInfo,
      used);
}


inline void Allocator::removeFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::removeFramework,
      frameworkId);
}


inline void Allocator::activateFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::activateFramework,
      frameworkId);
}


inline void Allocator::deactivateFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::deactivateFramework,
      frameworkId);
}


inline void Allocator::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  process::dispatch(
      process,
      &AllocatorProcess::addSlave,
      slaveId,
      slaveInfo,
      total,
      used);
}


inline void Allocator::removeSlave(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::removeSlave,
      slaveId);
}


inline void Allocator::activateSlave(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::activateSlave,
      slaveId);
}


inline void Allocator::deactivateSlave(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::deactivateSlave,
      slaveId);
}


inline void Allocator::updateWhitelist(
    const Option<hashset<std::string> >& whitelist)
{
  process::dispatch(
      process,
      &AllocatorProcess::updateWhitelist,
      whitelist);
}


inline void Allocator::requestResources(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  process::dispatch(
      process,
      &AllocatorProcess::requestResources,
      frameworkId,
      requests);
}


inline void Allocator::updateAllocation(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const std::vector<Offer::Operation>& operations)
{
  process::dispatch(
      process,
      &AllocatorProcess::updateAllocation,
      frameworkId,
      slaveId,
      operations);
}


inline void Allocator::recoverResources(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters)
{
  process::dispatch(
      process,
      &AllocatorProcess::recoverResources,
      frameworkId,
      slaveId,
      resources,
      filters);
}


inline void Allocator::reviveOffers(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::reviveOffers,
      frameworkId);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __ALLOCATOR_HPP__
