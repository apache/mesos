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

#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/dispatch.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace master {

class Master; // Forward declaration.

namespace allocator {

// Basic model of an allocator: resources are allocated to a framework
// in the form of offers. A framework can refuse some resources in
// offers and run tasks in others. Resources can be recovered from a
// framework when tasks finish/fail (or are lost due to a slave
// failure) or when an offer is rescinded.
// NOTE: New Allocators should implement this interface.
class AllocatorProcess : public process::Process<AllocatorProcess>
{
public:
  AllocatorProcess() {}

  virtual ~AllocatorProcess() {}

  // Explicitely use 'initialize' since we're overloading below.
  using process::ProcessBase::initialize;

  virtual void initialize(
      const Flags& flags,
      const process::PID<Master>& master,
      const hashmap<std::string, RoleInfo>& roles) = 0;

  virtual void frameworkAdded(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used) = 0;

  virtual void frameworkRemoved(
      const FrameworkID& frameworkId) = 0;

  virtual void frameworkActivated(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo) = 0;

  virtual void frameworkDeactivated(
      const FrameworkID& frameworkId) = 0;

  virtual void slaveAdded(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void slaveRemoved(
      const SlaveID& slaveId) = 0;

  // No longer offers resources for the disconnected slave.
  virtual void slaveDisconnected(
      const SlaveID& slaveId) = 0;

  // Resumes resource offers for the reconnected slave.
  virtual void slaveReconnected(
      const SlaveID& slaveId) = 0;

  virtual void updateWhitelist(
      const Option<hashset<std::string> >& whitelist) = 0;

  virtual void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests) = 0;

  // Whenever resources offered to a framework go unused (e.g.,
  // refused) the master invokes this callback.
  virtual void resourcesUnused(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters) = 0;

  // Whenever resources are "recovered" in the cluster (e.g., a task
  // finishes, an offer is removed because a framework has failed or
  // is failing over) the master invokes this callback.
  virtual void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources) = 0;

  // Whenever a framework that has filtered resources wants to revive
  // offers for those resources the master invokes this callback.
  virtual void offersRevived(
      const FrameworkID& frameworkId) = 0;
};


// This is a wrapper around the AllocatorProcess interface.
// NOTE: DO NOT subclass this class when implementing a new allocator.
// Implement AllocatorProcess (above) instead!
class Allocator
{
public:
  // The AllocatorProcess object passed to the constructor is
  // spawned and terminated by the allocator. But it is the responsibility
  // of the caller to de-allocate the object, if necessary.
  explicit Allocator(AllocatorProcess* _process);

  virtual ~Allocator();

  void initialize(
      const Flags& flags,
      const process::PID<Master>& master,
      const hashmap<std::string, RoleInfo>& roles);

  void frameworkAdded(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const Resources& used);

  void frameworkRemoved(
      const FrameworkID& frameworkId);

  void frameworkActivated(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo);

  void frameworkDeactivated(
      const FrameworkID& frameworkId);

  void slaveAdded(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const hashmap<FrameworkID, Resources>& used);

  void slaveRemoved(
      const SlaveID& slaveId);

  void slaveDisconnected(
      const SlaveID& slaveId);

  void slaveReconnected(
      const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string> >& whitelist);

  void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void resourcesUnused(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources);

  void offersRevived(
      const FrameworkID& frameworkId);

private:
  Allocator(const Allocator&); // Not copyable.
  Allocator& operator=(const Allocator&); // Not assignable.

  AllocatorProcess* process;
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
    const process::PID<Master>& master,
    const hashmap<std::string, RoleInfo>& roles)
{
  process::dispatch(
      process,
      &AllocatorProcess::initialize,
      flags,
      master,
      roles);
}


inline void Allocator::frameworkAdded(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  process::dispatch(
      process,
      &AllocatorProcess::frameworkAdded,
      frameworkId,
      frameworkInfo,
      used);
}


inline void Allocator::frameworkRemoved(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::frameworkRemoved,
      frameworkId);
}


inline void Allocator::frameworkActivated(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  process::dispatch(
      process,
      &AllocatorProcess::frameworkActivated,
      frameworkId,
      frameworkInfo);
}


inline void Allocator::frameworkDeactivated(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::frameworkDeactivated,
      frameworkId);
}


inline void Allocator::slaveAdded(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const hashmap<FrameworkID, Resources>& used)
{
  process::dispatch(
      process,
      &AllocatorProcess::slaveAdded,
      slaveId,
      slaveInfo,
      used);
}


inline void Allocator::slaveRemoved(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::slaveRemoved,
      slaveId);
}


inline void Allocator::slaveDisconnected(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::slaveDisconnected,
      slaveId);
}


inline void Allocator::slaveReconnected(const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &AllocatorProcess::slaveReconnected,
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


inline void Allocator::resourcesRequested(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  process::dispatch(
      process,
      &AllocatorProcess::resourcesRequested,
      frameworkId,
      requests);
}


inline void Allocator::resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters)
{
  process::dispatch(
      process,
      &AllocatorProcess::resourcesUnused,
      frameworkId,
      slaveId,
      resources,
      filters);
}


inline void Allocator::resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  process::dispatch(
      process,
      &AllocatorProcess::resourcesRecovered,
      frameworkId,
      slaveId,
      resources);
}


inline void Allocator::offersRevived(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &AllocatorProcess::offersRevived,
      frameworkId);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __ALLOCATOR_HPP__
