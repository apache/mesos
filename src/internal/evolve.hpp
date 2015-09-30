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

#ifndef __INTERNAL_EVOLVE_HPP__
#define __INTERNAL_EVOLVE_HPP__

#include <google/protobuf/message.h>

#include <mesos/mesos.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <stout/foreach.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {

// Helpers for evolving types between versions. Please add as necessary!
v1::AgentID evolve(const SlaveID& slaveId);
v1::AgentInfo evolve(const SlaveInfo& slaveInfo);
v1::FrameworkID evolve(const FrameworkID& frameworkId);
v1::ExecutorID evolve(const ExecutorID& executorId);
v1::Offer evolve(const Offer& offer);
v1::InverseOffer evolve(const InverseOffer& inverseOffer);
v1::OfferID evolve(const OfferID& offerId);
v1::TaskInfo evolve(const TaskInfo& taskInfo);
v1::TaskStatus evolve(const TaskStatus& status);

v1::scheduler::Call evolve(const scheduler::Call& call);


// Helper for repeated field evolving to 'T1' from 'T2'.
template <typename T1, typename T2>
google::protobuf::RepeatedPtrField<T1> evolve(
    google::protobuf::RepeatedPtrField<T2> t2s)
{
  google::protobuf::RepeatedPtrField<T1> t1s;

  foreach (const T2& t2, t2s) {
    t1s.Add()->CopyFrom(evolve(t2));
  }

  return t1s;
}


v1::scheduler::Event evolve(const scheduler::Event& event);


// Helper functions that evolve old style internal messages to a
// v1::scheduler::Event.
v1::scheduler::Event evolve(const FrameworkRegisteredMessage& message);
v1::scheduler::Event evolve(const FrameworkReregisteredMessage& message);
v1::scheduler::Event evolve(const ResourceOffersMessage& message);
v1::scheduler::Event evolve(const RescindResourceOfferMessage& message);
v1::scheduler::Event evolve(const StatusUpdateMessage& message);
v1::scheduler::Event evolve(const LostSlaveMessage& message);
v1::scheduler::Event evolve(const ExitedExecutorMessage& message);
v1::scheduler::Event evolve(const ExecutorToFrameworkMessage& message);
v1::scheduler::Event evolve(const FrameworkErrorMessage& message);

} // namespace internal {
} // namespace mesos {

#endif // __INTERNAL_EVOLVE_HPP__
