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

#ifndef __TESTS_ALLOCATOR_HPP__
#define __TESTS_ALLOCATOR_HPP__

#include <gmock/gmock.h>

#include <mesos/allocator/allocator.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>

#include "master/allocator/mesos/hierarchical.hpp"

using ::testing::_;
using ::testing::An;
using ::testing::DoDefault;
using ::testing::Invoke;
using ::testing::Return;

using mesos::allocator::Options;

namespace mesos {
namespace internal {
namespace tests {

// Allocator test helpers.

Quota createQuota(const std::string& guarantees, const std::string& limits);


// This is a legacy helper where we take in a resource string
// and use that to set both quota guarantees and limits.
Quota createQuota(const std::string& resources);


WeightInfo createWeightInfo(const std::string& role, double weight);


// The following actions make up for the fact that DoDefault
// cannot be used inside a DoAll, for example:
// EXPECT_CALL(allocator, addFramework(_, _, _, _, _))
//   .WillOnce(DoAll(InvokeAddFramework(&allocator),
//                   FutureSatisfy(&addFramework)));

ACTION_P(InvokeInitialize, allocator)
{
  allocator->real->initialize(arg0, arg1, arg2);
}


ACTION_P(InvokeRecover, allocator)
{
  allocator->real->recover(arg0, arg1);
}


ACTION_P(InvokeAddFramework, allocator)
{
  allocator->real->addFramework(arg0, arg1, arg2, arg3, std::move(arg4));
}


ACTION_P(InvokeRemoveFramework, allocator)
{
  allocator->real->removeFramework(arg0);
}


ACTION_P(InvokeActivateFramework, allocator)
{
  allocator->real->activateFramework(arg0);
}


ACTION_P(InvokeDeactivateFramework, allocator)
{
  allocator->real->deactivateFramework(arg0);
}


ACTION_P(InvokeUpdateFramework, allocator)
{
  allocator->real->updateFramework(arg0, arg1, std::move(arg2));
}


ACTION_P(InvokeAddSlave, allocator)
{
  allocator->real->addSlave(arg0, arg1, arg2, arg3, arg4, arg5);
}


ACTION_P(InvokeRemoveSlave, allocator)
{
  allocator->real->removeSlave(arg0);
}


ACTION_P(InvokeUpdateSlave, allocator)
{
  allocator->real->updateSlave(arg0, arg1, arg2, arg3);
}


ACTION_P(InvokeAddResourceProvider, allocator)
{
  allocator->real->addResourceProvider(arg0, arg1, arg2);
}


ACTION_P(InvokeActivateSlave, allocator)
{
  allocator->real->activateSlave(arg0);
}


ACTION_P(InvokeDeactivateSlave, allocator)
{
  allocator->real->deactivateSlave(arg0);
}


ACTION_P(InvokeUpdateWhitelist, allocator)
{
  allocator->real->updateWhitelist(arg0);
}


ACTION_P(InvokeRequestResources, allocator)
{
  allocator->real->requestResources(arg0, arg1);
}


ACTION_P(InvokeUpdateAllocation, allocator)
{
  allocator->real->updateAllocation(arg0, arg1, arg2, arg3);
}


ACTION_P(InvokeUpdateAvailable, allocator)
{
  return allocator->real->updateAvailable(arg0, arg1);
}


ACTION_P(InvokeUpdateUnavailability, allocator)
{
  return allocator->real->updateUnavailability(arg0, arg1);
}


ACTION_P(InvokeUpdateInverseOffer, allocator)
{
  return allocator->real->updateInverseOffer(arg0, arg1, arg2, arg3, arg4);
}


ACTION_P(InvokeGetInverseOfferStatuses, allocator)
{
  return allocator->real->getInverseOfferStatuses();
}


ACTION_P(InvokeTransitionOfferedToAllocated, allocator)
{
  allocator->real->transitionOfferedToAllocated(arg0, arg1);
}


ACTION_P(InvokeRecoverResources, allocator)
{
  allocator->real->recoverResources(arg0, arg1, arg2, arg3, arg4);
}


ACTION_P2(InvokeRecoverResourcesWithFilters, allocator, timeout)
{
  Filters filters;
  filters.set_refuse_seconds(timeout);

  allocator->real->recoverResources(arg0, arg1, arg2, filters, false);
}


ACTION_P(InvokeSuppressOffers, allocator)
{
  allocator->real->suppressOffers(arg0, arg1);
}


ACTION_P(InvokeReviveOffers, allocator)
{
  allocator->real->reviveOffers(arg0, arg1);
}


ACTION_P(InvokeUpdateQuota, allocator)
{
  allocator->real->updateQuota(arg0, arg1);
}


ACTION_P(InvokeUpdateWeights, allocator)
{
  allocator->real->updateWeights(arg0);
}


ACTION_P(InvokePause, allocator)
{
  allocator->real->pause();
}


ACTION_P(InvokeResume, allocator)
{
  allocator->real->resume();
}


template <typename T = master::allocator::HierarchicalDRFAllocator>
mesos::allocator::Allocator* createAllocator()
{
  // T represents the allocator type. It can be a default built-in
  // allocator, or one provided by an allocator module.
  Try<mesos::allocator::Allocator*> instance = T::create();
  CHECK_SOME(instance);
  return CHECK_NOTNULL(instance.get());
}

template <typename T = master::allocator::HierarchicalDRFAllocator>
class TestAllocator : public mesos::allocator::Allocator
{
public:
  // Actual allocation is done by an instance of real allocator,
  // which is specified by the template parameter.
  TestAllocator() : real(createAllocator<T>())
  {
    // We use 'ON_CALL' and 'WillByDefault' here to specify the
    // default actions (call in to the real allocator). This allows
    // the tests to leverage the 'DoDefault' action.
    // However, 'ON_CALL' results in a "Uninteresting mock function
    // call" warning unless each test puts expectations in place.
    // As a result, we also use 'EXPECT_CALL' and 'WillRepeatedly'
    // to get the best of both worlds: the ability to use 'DoDefault'
    // and no warnings when expectations are not explicit.

    ON_CALL(*this, initialize(_, _, _))
      .WillByDefault(InvokeInitialize(this));
    EXPECT_CALL(*this, initialize(_, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, recover(_, _))
      .WillByDefault(InvokeRecover(this));
    EXPECT_CALL(*this, recover(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, addFramework_(_, _, _, _, _))
      .WillByDefault(InvokeAddFramework(this));
    EXPECT_CALL(*this, addFramework_(_, _, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, removeFramework(_))
      .WillByDefault(InvokeRemoveFramework(this));
    EXPECT_CALL(*this, removeFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, activateFramework(_))
      .WillByDefault(InvokeActivateFramework(this));
    EXPECT_CALL(*this, activateFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, deactivateFramework(_))
      .WillByDefault(InvokeDeactivateFramework(this));
    EXPECT_CALL(*this, deactivateFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateFramework_(_, _, _))
      .WillByDefault(InvokeUpdateFramework(this));
    EXPECT_CALL(*this, updateFramework_( _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, addSlave(_, _, _, _, _, _))
      .WillByDefault(InvokeAddSlave(this));
    EXPECT_CALL(*this, addSlave(_, _, _, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, removeSlave(_))
      .WillByDefault(InvokeRemoveSlave(this));
    EXPECT_CALL(*this, removeSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateSlave(_, _, _, _))
      .WillByDefault(InvokeUpdateSlave(this));
    EXPECT_CALL(*this, updateSlave(_, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, addResourceProvider(_, _, _))
      .WillByDefault(InvokeAddResourceProvider(this));
    EXPECT_CALL(*this, addResourceProvider(_, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, activateSlave(_))
      .WillByDefault(InvokeActivateSlave(this));
    EXPECT_CALL(*this, activateSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, deactivateSlave(_))
      .WillByDefault(InvokeDeactivateSlave(this));
    EXPECT_CALL(*this, deactivateSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateWhitelist(_))
      .WillByDefault(InvokeUpdateWhitelist(this));
    EXPECT_CALL(*this, updateWhitelist(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, requestResources(_, _))
      .WillByDefault(InvokeRequestResources(this));
    EXPECT_CALL(*this, requestResources(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateAllocation(_, _, _, _))
      .WillByDefault(InvokeUpdateAllocation(this));
    EXPECT_CALL(*this, updateAllocation(_, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateAvailable(_, _))
      .WillByDefault(InvokeUpdateAvailable(this));
    EXPECT_CALL(*this, updateAvailable(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateUnavailability(_, _))
      .WillByDefault(InvokeUpdateUnavailability(this));
    EXPECT_CALL(*this, updateUnavailability(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateInverseOffer(_, _, _, _, _))
      .WillByDefault(InvokeUpdateInverseOffer(this));
    EXPECT_CALL(*this, updateInverseOffer(_, _, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, getInverseOfferStatuses())
      .WillByDefault(InvokeGetInverseOfferStatuses(this));
    EXPECT_CALL(*this, getInverseOfferStatuses())
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, transitionOfferedToAllocated(_, _))
      .WillByDefault(InvokeTransitionOfferedToAllocated(this));
    EXPECT_CALL(*this, transitionOfferedToAllocated(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, recoverResources(_, _, _, _, _))
      .WillByDefault(InvokeRecoverResources(this));
    EXPECT_CALL(*this, recoverResources(_, _, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, suppressOffers(_, _))
      .WillByDefault(InvokeSuppressOffers(this));
    EXPECT_CALL(*this, suppressOffers(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, reviveOffers(_, _))
      .WillByDefault(InvokeReviveOffers(this));
    EXPECT_CALL(*this, reviveOffers(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateQuota(_, _))
      .WillByDefault(InvokeUpdateQuota(this));
    EXPECT_CALL(*this, updateQuota(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateWeights(_))
      .WillByDefault(InvokeUpdateWeights(this));
    EXPECT_CALL(*this, updateWeights(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, pause())
      .WillByDefault(InvokePause(this));
    EXPECT_CALL(*this, pause())
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, resume())
      .WillByDefault(InvokeResume(this));
    EXPECT_CALL(*this, resume())
      .WillRepeatedly(DoDefault());
  }

  ~TestAllocator() override {}

  MOCK_METHOD3(initialize, void(
      const Options& options,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&));

  MOCK_METHOD2(recover, void(
      const int expectedAgentCount,
      const hashmap<std::string, Quota>&));

  // NOTE: Due to gmock's limitations, instead of an rvlalue reference,
  // a non-const lvalue reference to FrameworkOptions is passed into
  // the mock method.
  MOCK_METHOD5(addFramework_, void(
      const FrameworkID&,
      const FrameworkInfo&,
      const hashmap<SlaveID, Resources>&,
      bool active,
      ::mesos::allocator::FrameworkOptions&));

  void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used,
      bool active,
      ::mesos::allocator::FrameworkOptions&& options) override
  {
    addFramework_(frameworkId, frameworkInfo, used, active, options);
  };

  MOCK_METHOD1(removeFramework, void(
      const FrameworkID&));

  MOCK_METHOD1(activateFramework, void(
      const FrameworkID&));

  MOCK_METHOD1(deactivateFramework, void(
      const FrameworkID&));

  // NOTE: Due to gmock's limitations, instead of an rvlalue reference,
  // a non-const lvalue reference to FrameworkOptions is passed into
  // the mock method.
  MOCK_METHOD3(updateFramework_, void(
      const FrameworkID&,
      const FrameworkInfo&,
      ::mesos::allocator::FrameworkOptions&));

  void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      ::mesos::allocator::FrameworkOptions&& options) override
  {
    updateFramework_(frameworkId, frameworkInfo, options);
  };

  MOCK_METHOD6(addSlave, void(
      const SlaveID&,
      const SlaveInfo&,
      const std::vector<SlaveInfo::Capability>&,
      const Option<Unavailability>&,
      const Resources&,
      const hashmap<FrameworkID, Resources>&));

  MOCK_METHOD1(removeSlave, void(
      const SlaveID&));

  MOCK_METHOD4(updateSlave, void(
      const SlaveID&,
      const SlaveInfo&,
      const Option<Resources>&,
      const Option<std::vector<SlaveInfo::Capability>>&));

  MOCK_METHOD3(addResourceProvider, void(
      const SlaveID&,
      const Resources&,
      const hashmap<FrameworkID, Resources>&));

  MOCK_METHOD1(activateSlave, void(
      const SlaveID&));

  MOCK_METHOD1(deactivateSlave, void(
      const SlaveID&));

  MOCK_METHOD1(updateWhitelist, void(
      const Option<hashset<std::string>>&));

  MOCK_METHOD2(requestResources, void(
      const FrameworkID&,
      const std::vector<Request>&));

  MOCK_METHOD4(updateAllocation, void(
      const FrameworkID&,
      const SlaveID&,
      const Resources&,
      const std::vector<ResourceConversion>&));

  MOCK_METHOD2(updateAvailable, process::Future<Nothing>(
      const SlaveID&,
      const std::vector<Offer::Operation>&));

  MOCK_METHOD2(updateUnavailability, void(
      const SlaveID&,
      const Option<Unavailability>&));

  MOCK_METHOD5(updateInverseOffer, void(
      const SlaveID&,
      const FrameworkID&,
      const Option<UnavailableResources>&,
      const Option<mesos::allocator::InverseOfferStatus>&,
      const Option<Filters>&));

  MOCK_METHOD0(getInverseOfferStatuses, process::Future<
      hashmap<SlaveID, hashmap<
          FrameworkID,
          mesos::allocator::InverseOfferStatus>>>());

  MOCK_METHOD2(transitionOfferedToAllocated, void(
      const SlaveID&,
      const Resources&));

  MOCK_METHOD5(recoverResources, void(
      const FrameworkID&,
      const SlaveID&,
      const Resources&,
      const Option<Filters>& filters,
      bool isAllocated));

  MOCK_METHOD2(suppressOffers, void(
      const FrameworkID&,
      const std::set<std::string>&));

  MOCK_METHOD2(reviveOffers, void(
      const FrameworkID&,
      const std::set<std::string>&));

  MOCK_METHOD2(updateQuota, void(
      const std::string&,
      const Quota&));

  MOCK_METHOD1(updateWeights, void(
      const std::vector<WeightInfo>&));

  MOCK_METHOD0(pause, void());

  MOCK_METHOD0(resume, void());

  process::Owned<mesos::allocator::Allocator> real;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_ALLOCATOR_HPP__
