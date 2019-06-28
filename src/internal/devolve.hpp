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

#ifndef __INTERNAL_DEVOLVE_HPP__
#define __INTERNAL_DEVOLVE_HPP__

#include <google/protobuf/message.h>

#include <mesos/agent/agent.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/master/master.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/v1/master/master.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <stout/foreach.hpp>

namespace mesos {
namespace internal {

// Helpers for devolving types between versions. Please add as necessary!
CommandInfo devolve(const v1::CommandInfo& command);
ContainerID devolve(const v1::ContainerID& containerId);
Credential devolve(const v1::Credential& credential);
DrainConfig devolve(const v1::DrainConfig& drainConfig);
DrainInfo devolve(const v1::DrainInfo& drainInfo);
DurationInfo devolve(const google::protobuf::Duration& duration);
ExecutorID devolve(const v1::ExecutorID& executorId);
FrameworkID devolve(const v1::FrameworkID& frameworkId);
FrameworkInfo devolve(const v1::FrameworkInfo& frameworkInfo);
HealthCheck devolve(const v1::HealthCheck& check);
InverseOffer devolve(const v1::InverseOffer& inverseOffer);
Offer devolve(const v1::Offer& offer);
Offer::Operation devolve(const v1::Offer::Operation& operation);
OperationStatus devolve(const v1::OperationStatus& status);
Resource devolve(const v1::Resource& resource);
ResourceProviderID devolve(const v1::ResourceProviderID& resourceProviderId);
ResourceProviderInfo devolve(
    const v1::ResourceProviderInfo& resourceProviderInfo);
Resources devolve(const v1::Resources& resources);
SlaveID devolve(const v1::AgentID& agentId);
SlaveInfo devolve(const v1::AgentInfo& agentInfo);
TaskID devolve(const v1::TaskID& taskId);
TaskStatus devolve(const v1::TaskStatus& status);

mesos::resource_provider::Call devolve(const v1::resource_provider::Call& call);
mesos::resource_provider::Event devolve(
    const v1::resource_provider::Event& event);

mesos::scheduler::Call devolve(const v1::scheduler::Call& call);
mesos::scheduler::Event devolve(const v1::scheduler::Event& event);

executor::Call devolve(const v1::executor::Call& call);
executor::Event devolve(const v1::executor::Event& event);

mesos::agent::Call devolve(const v1::agent::Call& call);
mesos::agent::Response devolve(const v1::agent::Response& response);

mesos::master::Call devolve(const v1::master::Call& call);

// Helper for repeated field devolving to 'T1' from 'T2'.
template <typename T1, typename T2>
google::protobuf::RepeatedPtrField<T1> devolve(
    google::protobuf::RepeatedPtrField<T2> t2s)
{
  google::protobuf::RepeatedPtrField<T1> t1s;

  foreach (const T2& t2, t2s) {
    t1s.Add()->CopyFrom(devolve(t2));
  }

  return t1s;
}

} // namespace internal {
} // namespace mesos {

#endif // __INTERNAL_DEVOLVE_HPP__
