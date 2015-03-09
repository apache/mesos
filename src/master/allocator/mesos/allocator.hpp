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

#ifndef __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__
#define __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include "master/allocator/allocator.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

class MesosAllocatorProcess;

// A wrapper for Process-based allocators. It redirects all function
// invocations to the underlying AllocatorProcess and manages its
// lifetime. We ensure the template parameter AllocatorProcess
// implements MesosAllocatorProcess by storing a pointer to it.
template <typename AllocatorProcess>
class MesosAllocator : public Allocator
{
public:
  MesosAllocator();

  ~MesosAllocator();

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

  void activateFramework(
      const FrameworkID& frameworkId);

  void deactivateFramework(
      const FrameworkID& frameworkId);

  void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used);

  void removeSlave(
      const SlaveID& slaveId);

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

  void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void reviveOffers(
      const FrameworkID& frameworkId);

private:
  MesosAllocator(const MesosAllocator&); // Not copyable.
  MesosAllocator& operator=(const MesosAllocator&); // Not assignable.

  MesosAllocatorProcess* process;
};


// The basic interface for all Process-based allocators.
class MesosAllocatorProcess : public process::Process<MesosAllocatorProcess>
{
public:
  MesosAllocatorProcess() {}

  virtual ~MesosAllocatorProcess() {}

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


template <typename AllocatorProcess>
MesosAllocator<AllocatorProcess>::MesosAllocator()
{
  process = new AllocatorProcess();
  process::spawn(process);
}


template <typename AllocatorProcess>
MesosAllocator<AllocatorProcess>::~MesosAllocator()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::initialize(
    const Flags& flags,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, Resources>&)>& offerCallback,
    const hashmap<std::string, RoleInfo>& roles)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::initialize,
      flags,
      offerCallback,
      roles);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::addFramework,
      frameworkId,
      frameworkInfo,
      used);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::removeFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::removeFramework,
      frameworkId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::activateFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::activateFramework,
      frameworkId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::deactivateFramework(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::deactivateFramework,
      frameworkId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::addSlave,
      slaveId,
      slaveInfo,
      total,
      used);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::removeSlave(
    const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::removeSlave,
      slaveId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::activateSlave(
    const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::activateSlave,
      slaveId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::deactivateSlave(
    const SlaveID& slaveId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::deactivateSlave,
      slaveId);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::updateWhitelist(
    const Option<hashset<std::string> >& whitelist)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateWhitelist,
      whitelist);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::requestResources(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::requestResources,
      frameworkId,
      requests);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::updateAllocation(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const std::vector<Offer::Operation>& operations)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateAllocation,
      frameworkId,
      slaveId,
      operations);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::recoverResources(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::recoverResources,
      frameworkId,
      slaveId,
      resources,
      filters);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::reviveOffers(
    const FrameworkID& frameworkId)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::reviveOffers,
      frameworkId);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__
