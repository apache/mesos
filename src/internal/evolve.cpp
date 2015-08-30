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

#include <process/pid.hpp>

#include <stout/check.hpp>

#include "internal/evolve.hpp"

using std::string;

using process::UPID;

namespace mesos {
namespace internal {

// Helper for evolving a type by serializing/parsing when the types
// have not changed across versions.
template <typename T>
static T evolve(const google::protobuf::Message& message)
{
  T t;

  string data;

  // NOTE: We need to use 'SerializePartialToString' instead of
  // 'SerializeToString' because some required fields might not be set
  // and we don't want an exception to get thrown.
  CHECK(message.SerializePartialToString(&data))
    << "Failed to serialize " << message.GetTypeName()
    << " while evolving to " << t.GetTypeName();

  // NOTE: We need to use 'ParsePartialFromString' instead of
  // 'ParsePartialFromString' because some required fields might not
  // be set and we don't want an exception to get thrown.
  CHECK(t.ParsePartialFromString(data))
    << "Failed to parse " << t.GetTypeName()
    << " while evolving from " << message.GetTypeName();

  return t;
}


v1::AgentID evolve(const SlaveID& slaveId)
{
  // NOTE: Not using 'evolve<SlaveID, v1::AgentID>(slaveId)' since
  // this will be a common 'evolve' call and we wanted to speed up
  // performance.

  v1::AgentID id;
  id.set_value(slaveId.value());
  return id;
}


v1::AgentInfo evolve(const SlaveInfo& slaveInfo)
{
  return evolve<v1::AgentInfo>(slaveInfo);
}


v1::FrameworkID evolve(const FrameworkID& frameworkId)
{
  return evolve<v1::FrameworkID>(frameworkId);
}


v1::ExecutorID evolve(const ExecutorID& executorId)
{
  return evolve<v1::ExecutorID>(executorId);
}


v1::Offer evolve(const Offer& offer)
{
  return evolve<v1::Offer>(offer);
}


v1::InverseOffer evolve(const InverseOffer& inverseOffer)
{
  return evolve<v1::InverseOffer>(inverseOffer);
}


v1::OfferID evolve(const OfferID& offerId)
{
  return evolve<v1::OfferID>(offerId);
}


v1::TaskInfo evolve(const TaskInfo& taskInfo)
{
  return evolve<v1::TaskInfo>(taskInfo);
}


v1::TaskStatus evolve(const TaskStatus& status)
{
  return evolve<v1::TaskStatus>(status);
}


v1::scheduler::Call evolve(const scheduler::Call& call)
{
  return evolve<v1::scheduler::Call>(call);
}


v1::scheduler::Event evolve(const scheduler::Event& event)
{
  return evolve<v1::scheduler::Event>(event);
}


v1::scheduler::Event evolve(const FrameworkRegisteredMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::SUBSCRIBED);

  v1::scheduler::Event::Subscribed* subscribed = event.mutable_subscribed();
  subscribed->mutable_framework_id()->CopyFrom(evolve(message.framework_id()));

  return event;
}


v1::scheduler::Event evolve(const FrameworkReregisteredMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::SUBSCRIBED);

  v1::scheduler::Event::Subscribed* subscribed = event.mutable_subscribed();
  subscribed->mutable_framework_id()->CopyFrom(evolve(message.framework_id()));

  return event;
}


v1::scheduler::Event evolve(const ResourceOffersMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::OFFERS);

  v1::scheduler::Event::Offers* offers = event.mutable_offers();
  offers->mutable_offers()->CopyFrom(evolve<v1::Offer>(message.offers()));
  offers->mutable_inverse_offers()->CopyFrom(evolve<v1::InverseOffer>(
      message.inverse_offers()));

  return event;
}


v1::scheduler::Event evolve(const RescindResourceOfferMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::RESCIND);

  v1::scheduler::Event::Rescind* rescind = event.mutable_rescind();
  rescind->mutable_offer_id()->CopyFrom(evolve(message.offer_id()));

  return event;
}


v1::scheduler::Event evolve(const StatusUpdateMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::UPDATE);

  v1::scheduler::Event::Update* update = event.mutable_update();

  update->mutable_status()->CopyFrom(evolve(message.update().status()));

  if (message.update().has_slave_id()) {
    update->mutable_status()->mutable_agent_id()->CopyFrom(
        evolve(message.update().slave_id()));
  }

  if (message.update().has_executor_id()) {
    update->mutable_status()->mutable_executor_id()->CopyFrom(
        evolve(message.update().executor_id()));
  }

  update->mutable_status()->set_timestamp(message.update().timestamp());

  // If the update does not have a 'uuid', it does not need
  // acknowledging. However, prior to 0.23.0, the update uuid
  // was required and always set. In 0.24.0, we can rely on the
  // update uuid check here, until then we must still check for
  // this being sent from the driver (from == UPID()) or from
  // the master (pid == UPID()).
  // TODO(vinod): Get rid of this logic in 0.25.0 because master
  // and slave correctly set task status in 0.24.0.
  if (!message.update().has_uuid() || message.update().uuid() == "") {
    update->mutable_status()->clear_uuid();
  } else if (UPID(message.pid()) == UPID()) {
    update->mutable_status()->clear_uuid();
  } else {
    update->mutable_status()->set_uuid(message.update().uuid());
  }

  return event;
}


v1::scheduler::Event evolve(const LostSlaveMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::FAILURE);

  v1::scheduler::Event::Failure* failure = event.mutable_failure();
  failure->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));

  return event;
}


v1::scheduler::Event evolve(const ExitedExecutorMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::FAILURE);

  v1::scheduler::Event::Failure* failure = event.mutable_failure();
  failure->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));
  failure->mutable_executor_id()->CopyFrom(evolve(message.executor_id()));
  failure->set_status(message.status());

  return event;
}


v1::scheduler::Event evolve(const ExecutorToFrameworkMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::MESSAGE);

  v1::scheduler::Event::Message* message_ = event.mutable_message();
  message_->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));
  message_->mutable_executor_id()->CopyFrom(evolve(message.executor_id()));
  message_->set_data(message.data());

  return event;
}


v1::scheduler::Event evolve(const FrameworkErrorMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::ERROR);

  v1::scheduler::Event::Error* error = event.mutable_error();
  error->set_message(message.message());

  return event;
}

} // namespace internal {
} // namespace mesos {
