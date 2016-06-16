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

#include <mesos/executor/executor.hpp>

#include <mesos/master/master.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/v1/master/master.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <stout/foreach.hpp>

namespace mesos {
namespace internal {

// Helpers for devolving types between versions. Please add as necessary!
SlaveID devolve(const v1::AgentID& agentId);
SlaveInfo devolve(const v1::AgentInfo& agentInfo);
FrameworkID devolve(const v1::FrameworkID& frameworkId);
FrameworkInfo devolve(const v1::FrameworkInfo& frameworkInfo);
ExecutorID devolve(const v1::ExecutorID& executorId);
Offer devolve(const v1::Offer& offer);
InverseOffer devolve(const v1::InverseOffer& inverseOffer);
Credential devolve(const v1::Credential& credential);
TaskStatus devolve(const v1::TaskStatus& status);

scheduler::Call devolve(const v1::scheduler::Call& call);
scheduler::Event devolve(const v1::scheduler::Event& event);

executor::Call devolve(const v1::executor::Call& call);

mesos::agent::Call devolve(const v1::agent::Call& call);

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
