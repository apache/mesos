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

#include <stout/check.hpp>

#include "internal/devolve.hpp"

using std::string;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace internal {

template <typename T>
static T devolve(const google::protobuf::Message& message)
{
  T t;

  string data;

  // NOTE: We need to use 'SerializePartialToString' instead of
  // 'SerializeToString' because some required fields might not be set
  // and we don't want an exception to get thrown.
  CHECK(message.SerializePartialToString(&data))
    << "Failed to serialize " << message.GetTypeName()
    << " while devolving to " << t.GetTypeName();

  // NOTE: We need to use 'ParsePartialFromString' instead of
  // 'ParseFromString' because some required fields might not
  // be set and we don't want an exception to get thrown.
  CHECK(t.ParsePartialFromString(data))
    << "Failed to parse " << t.GetTypeName()
    << " while devolving from " << message.GetTypeName();

  return t;
}


CommandInfo devolve(const v1::CommandInfo& command)
{
  return devolve<CommandInfo>(command);
}


ContainerID devolve(const v1::ContainerID& containerId)
{
  return devolve<ContainerID>(containerId);
}


Credential devolve(const v1::Credential& credential)
{
  return devolve<Credential>(credential);
}


DrainConfig devolve(const v1::DrainConfig& drainConfig)
{
  return devolve<DrainConfig>(drainConfig);
}


DrainInfo devolve(const v1::DrainInfo& drainInfo)
{
  return devolve<DrainInfo>(drainInfo);
}


DurationInfo devolve(const google::protobuf::Duration& duration)
{
  DurationInfo durationInfo;

  // NOTE: If not specified, the fields of Duration default to zero.
  durationInfo.set_nanoseconds(
      duration.seconds() * 1000000000 + duration.nanos());

  return durationInfo;
}


ExecutorID devolve(const v1::ExecutorID& executorId)
{
  return devolve<ExecutorID>(executorId);
}


FrameworkID devolve(const v1::FrameworkID& frameworkId)
{
  return devolve<FrameworkID>(frameworkId);
}


FrameworkInfo devolve(const v1::FrameworkInfo& frameworkInfo)
{
  return devolve<FrameworkInfo>(frameworkInfo);
}


HealthCheck devolve(const v1::HealthCheck& check)
{
  return devolve<HealthCheck>(check);
}


InverseOffer devolve(const v1::InverseOffer& inverseOffer)
{
  return devolve<InverseOffer>(inverseOffer);
}


Offer devolve(const v1::Offer& offer)
{
  return devolve<Offer>(offer);
}


Offer::Operation devolve(const v1::Offer::Operation& operation)
{
  return devolve<Offer::Operation>(operation);
}


OperationStatus devolve(const v1::OperationStatus& status)
{
  OperationStatus _status = devolve<OperationStatus>(status);

  if (status.has_agent_id()) {
    *_status.mutable_slave_id() = devolve<SlaveID>(status.agent_id());
  }

  return _status;
}


Resource devolve(const v1::Resource& resource)
{
  return devolve<Resource>(resource);
}


ResourceProviderID devolve(const v1::ResourceProviderID& resourceProviderId)
{
  // NOTE: We do not use the common 'devolve' call for performance.
  ResourceProviderID id;
  id.set_value(resourceProviderId.value());
  return id;
}

ResourceProviderInfo devolve(
    const v1::ResourceProviderInfo& resourceProviderInfo)
{
  return devolve<ResourceProviderInfo>(resourceProviderInfo);
}


Resources devolve(const v1::Resources& resources)
{
  return devolve<Resource>(
      static_cast<const RepeatedPtrField<v1::Resource>&>(resources));
}


SlaveID devolve(const v1::AgentID& agentId)
{
  // NOTE: Not using 'devolve<v1::AgentID, SlaveID>(agentId)' since
  // this will be a common 'devolve' call and we wanted to speed up
  // performance.

  SlaveID id;
  id.set_value(agentId.value());
  return id;
}


SlaveInfo devolve(const v1::AgentInfo& agentInfo)
{
  SlaveInfo info = devolve<SlaveInfo>(agentInfo);

  // We set 'checkpoint' to 'true' since the v1::AgentInfo doesn't
  // have 'checkpoint' but all "agents" were checkpointing by default
  // when v1::AgentInfo was introduced. See MESOS-2317.
  info.set_checkpoint(true);

  return info;
}


TaskID devolve(const v1::TaskID& taskId)
{
  return devolve<TaskID>(taskId);
}


TaskStatus devolve(const v1::TaskStatus& status)
{
  return devolve<TaskStatus>(status);
}


executor::Call devolve(const v1::executor::Call& call)
{
  return devolve<executor::Call>(call);
}


executor::Event devolve(const v1::executor::Event& event)
{
  return devolve<executor::Event>(event);
}


mesos::resource_provider::Call devolve(const v1::resource_provider::Call& call)
{
  return devolve<mesos::resource_provider::Call>(call);
}


mesos::resource_provider::Event devolve(
    const v1::resource_provider::Event& event)
{
  return devolve<mesos::resource_provider::Event>(event);
}


scheduler::Call devolve(const v1::scheduler::Call& call)
{
  scheduler::Call _call = devolve<scheduler::Call>(call);

  // Certain conversions require special handling.
  if (call.type() == v1::scheduler::Call::SUBSCRIBE &&
      call.has_subscribe()) {
    // v1 Subscribe.suppressed_roles cannot be automatically converted
    // because its tag is used by another field in the internal Subscribe.
    *(_call.mutable_subscribe()->mutable_suppressed_roles()) =
      call.subscribe().suppressed_roles();

    *(_call.mutable_subscribe()->mutable_offer_constraints()) =
      devolve<scheduler::OfferConstraints>(
          call.subscribe().offer_constraints());
  }

  if (call.type() == v1::scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS &&
      call.has_acknowledge_operation_status() &&
      call.acknowledge_operation_status().has_agent_id()) {
    *_call.mutable_acknowledge_operation_status()->mutable_slave_id() =
      devolve(call.acknowledge_operation_status().agent_id());
  }

  return _call;
}


scheduler::Event devolve(const v1::scheduler::Event& event)
{
  return devolve<scheduler::Event>(event);
}


mesos::agent::Call devolve(const v1::agent::Call& call)
{
  return devolve<mesos::agent::Call>(call);
}


mesos::agent::Response devolve(const v1::agent::Response& response)
{
  return devolve<mesos::agent::Response>(response);
}


mesos::master::Call devolve(const v1::master::Call& call)
{
  mesos::master::Call _call = devolve<mesos::master::Call>(call);

  // The `google.protobuf.Duration` field in the `DrainAgent` call does not get
  // devolved automatically with the templated helper, so we devolve it
  // explicitly here.
  if (call.type() == v1::master::Call::DRAIN_AGENT &&
      call.has_drain_agent() &&
      call.drain_agent().has_max_grace_period()) {
    *_call.mutable_drain_agent()->mutable_max_grace_period() =
      devolve(call.drain_agent().max_grace_period());
  }

  return _call;
}

} // namespace internal {
} // namespace mesos {
