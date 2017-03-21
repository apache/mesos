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

#ifndef __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__
#define __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__

#include <mesos/allocator/allocator.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

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
class MesosAllocator : public mesos::allocator::Allocator
{
public:
  // Factory to allow for typed tests.
  static Try<mesos::allocator::Allocator*> create();

  ~MesosAllocator();

  void initialize(
      const Duration& allocationInterval,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&
                   offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback,
      const Option<std::set<std::string>>&
        fairnessExcludeResourceNames = None());

  void recover(
      const int expectedAgentCount,
      const hashmap<std::string, Quota>& quotas);

  void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used,
      bool active);

  void removeFramework(
      const FrameworkID& frameworkId);

  void activateFramework(
      const FrameworkID& frameworkId);

  void deactivateFramework(
      const FrameworkID& frameworkId);

  void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo);

  void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const std::vector<SlaveInfo::Capability>& capabilities,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used);

  void removeSlave(
      const SlaveID& slaveId);

  void updateSlave(
      const SlaveID& slave,
      const Option<Resources>& oversubscribed = None(),
      const Option<std::vector<SlaveInfo::Capability>>& capabilities = None());

  void activateSlave(
      const SlaveID& slaveId);

  void deactivateSlave(
      const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string>>& whitelist);

  void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& offeredResources,
      const std::vector<Offer::Operation>& operations);

  process::Future<Nothing> updateAvailable(
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations);

  void updateUnavailability(
      const SlaveID& slaveId,
      const Option<Unavailability>& unavailability);

  void updateInverseOffer(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const Option<UnavailableResources>& unavailableResources,
      const Option<mesos::allocator::InverseOfferStatus>& status,
      const Option<Filters>& filters);

  process::Future<
      hashmap<SlaveID,
              hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>>>
    getInverseOfferStatuses();

  void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters);

  void suppressOffers(
      const FrameworkID& frameworkId,
      const Option<std::string>& role);

  void reviveOffers(
      const FrameworkID& frameworkId,
      const Option<std::string>& role);

  void setQuota(
      const std::string& role,
      const Quota& quota);

  void removeQuota(
      const std::string& role);

  void updateWeights(
      const std::vector<WeightInfo>& weightInfos);

private:
  MesosAllocator();
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
      const Duration& allocationInterval,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&
                   offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback,
      const Option<std::set<std::string>>&
        fairnessExcludeResourceNames = None()) = 0;

  virtual void recover(
      const int expectedAgentCount,
      const hashmap<std::string, Quota>& quotas) = 0;

  virtual void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used,
      bool active) = 0;

  virtual void removeFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void activateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void deactivateFramework(
      const FrameworkID& frameworkId) = 0;

  virtual void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo) = 0;

  virtual void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const std::vector<SlaveInfo::Capability>& capabilities,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  virtual void removeSlave(
      const SlaveID& slaveId) = 0;

  virtual void updateSlave(
      const SlaveID& slave,
      const Option<Resources>& oversubscribed = None(),
      const Option<std::vector<SlaveInfo::Capability>>&
          capabilities = None()) = 0;

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
      const Resources& offeredResources,
      const std::vector<Offer::Operation>& operations) = 0;

  virtual process::Future<Nothing> updateAvailable(
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations) = 0;

  virtual void updateUnavailability(
      const SlaveID& slaveId,
      const Option<Unavailability>& unavailability) = 0;

  virtual void updateInverseOffer(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const Option<UnavailableResources>& unavailableResources,
      const Option<mesos::allocator::InverseOfferStatus>& status,
      const Option<Filters>& filters = None()) = 0;

  virtual process::Future<
      hashmap<SlaveID,
              hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>>>
    getInverseOfferStatuses() = 0;

  virtual void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters) = 0;

  virtual void suppressOffers(
      const FrameworkID& frameworkId,
      const Option<std::string>& role) = 0;

  virtual void reviveOffers(
      const FrameworkID& frameworkId,
      const Option<std::string>& role) = 0;

  virtual void setQuota(
      const std::string& role,
      const Quota& quota) = 0;

  virtual void removeQuota(
      const std::string& role) = 0;

  virtual void updateWeights(
      const std::vector<WeightInfo>& weightInfos) = 0;
};


template <typename AllocatorProcess>
Try<mesos::allocator::Allocator*>
MesosAllocator<AllocatorProcess>::create()
{
  mesos::allocator::Allocator* allocator =
    new MesosAllocator<AllocatorProcess>();
  return CHECK_NOTNULL(allocator);
}

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
    const Duration& allocationInterval,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&
                 offerCallback,
    const lambda::function<
        void(const FrameworkID&,
              const hashmap<SlaveID, UnavailableResources>&)>&
      inverseOfferCallback,
    const Option<std::set<std::string>>& fairnessExcludeResourceNames)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::initialize,
      allocationInterval,
      offerCallback,
      inverseOfferCallback,
      fairnessExcludeResourceNames);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::recover(
    const int expectedAgentCount,
    const hashmap<std::string, Quota>& quotas)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::recover,
      expectedAgentCount,
      quotas);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const hashmap<SlaveID, Resources>& used,
    bool active)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::addFramework,
      frameworkId,
      frameworkInfo,
      used,
      active);
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
inline void MesosAllocator<AllocatorProcess>::updateFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateFramework,
      frameworkId,
      frameworkInfo);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const std::vector<SlaveInfo::Capability>& capabilities,
    const Option<Unavailability>& unavailability,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::addSlave,
      slaveId,
      slaveInfo,
      capabilities,
      unavailability,
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
inline void MesosAllocator<AllocatorProcess>::updateSlave(
    const SlaveID& slaveId,
    const Option<Resources>& oversubscribed,
    const Option<std::vector<SlaveInfo::Capability>>& capabilities)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateSlave,
      slaveId,
      oversubscribed,
      capabilities);
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
    const Option<hashset<std::string>>& whitelist)
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
    const Resources& offeredResources,
    const std::vector<Offer::Operation>& operations)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateAllocation,
      frameworkId,
      slaveId,
      offeredResources,
      operations);
}


template <typename AllocatorProcess>
inline process::Future<Nothing>
MesosAllocator<AllocatorProcess>::updateAvailable(
    const SlaveID& slaveId,
    const std::vector<Offer::Operation>& operations)
{
  return process::dispatch(
      process,
      &MesosAllocatorProcess::updateAvailable,
      slaveId,
      operations);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::updateUnavailability(
    const SlaveID& slaveId,
    const Option<Unavailability>& unavailability)
{
  return process::dispatch(
      process,
      &MesosAllocatorProcess::updateUnavailability,
      slaveId,
      unavailability);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::updateInverseOffer(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Option<UnavailableResources>& unavailableResources,
    const Option<mesos::allocator::InverseOfferStatus>& status,
    const Option<Filters>& filters)
{
  return process::dispatch(
      process,
      &MesosAllocatorProcess::updateInverseOffer,
      slaveId,
      frameworkId,
      unavailableResources,
      status,
      filters);
}


template <typename AllocatorProcess>
inline process::Future<
    hashmap<SlaveID,
            hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>>>
  MesosAllocator<AllocatorProcess>::getInverseOfferStatuses()
{
  return process::dispatch(
      process,
      &MesosAllocatorProcess::getInverseOfferStatuses);
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
inline void MesosAllocator<AllocatorProcess>::suppressOffers(
    const FrameworkID& frameworkId,
    const Option<std::string>& role)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::suppressOffers,
      frameworkId,
      role);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::reviveOffers(
    const FrameworkID& frameworkId,
    const Option<std::string>& role)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::reviveOffers,
      frameworkId,
      role);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::setQuota(
    const std::string& role,
    const Quota& quota)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::setQuota,
      role,
      quota);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::removeQuota(
    const std::string& role)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::removeQuota,
      role);
}


template <typename AllocatorProcess>
inline void MesosAllocator<AllocatorProcess>::updateWeights(
    const std::vector<WeightInfo>& weightInfos)
{
  process::dispatch(
      process,
      &MesosAllocatorProcess::updateWeights,
      weightInfos);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_ALLOCATOR_HPP__
