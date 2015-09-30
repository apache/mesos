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

#include <stout/check.hpp>

#include "internal/devolve.hpp"

using std::string;

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
  // 'ParsePartialFromString' because some required fields might not
  // be set and we don't want an exception to get thrown.
  CHECK(t.ParsePartialFromString(data))
    << "Failed to parse " << t.GetTypeName()
    << " while devolving from " << message.GetTypeName();

  return t;
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
  // have 'checkpoint' but all "slaves" were checkpointing by default
  // when v1:;AgentInfo was introduced. See MESOS-2317.
  info.set_checkpoint(true);

  return info;
}


FrameworkID devolve(const v1::FrameworkID& frameworkId)
{
  return devolve<FrameworkID>(frameworkId);
}


FrameworkInfo devolve(const v1::FrameworkInfo& frameworkInfo)
{
  return devolve<FrameworkInfo>(frameworkInfo);
}


ExecutorID devolve(const v1::ExecutorID& executorId)
{
  return devolve<ExecutorID>(executorId);
}


Offer devolve(const v1::Offer& offer)
{
  return devolve<Offer>(offer);
}


InverseOffer devolve(const v1::InverseOffer& inverseOffer)
{
  return devolve<InverseOffer>(inverseOffer);
}


Credential devolve(const v1::Credential& credential)
{
  return devolve<Credential>(credential);
}


executor::Call devolve(const v1::executor::Call& call)
{
  return devolve<executor::Call>(call);
}


scheduler::Call devolve(const v1::scheduler::Call& call)
{
  return devolve<scheduler::Call>(call);
}


scheduler::Event devolve(const v1::scheduler::Event& event)
{
  return devolve<scheduler::Event>(event);
}

} // namespace internal {
} // namespace mesos {
