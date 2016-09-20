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

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>

#include <mesos/agent/agent.hpp>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "slave/slave.hpp"
#include "slave/validation.hpp"

namespace agent = mesos::agent;
namespace validation = mesos::internal::slave::validation;

using mesos::internal::slave::Slave;

namespace mesos {
namespace internal {
namespace tests {


TEST(AgentCallValidationTest, LaunchNestedContainer)
{
  // Missing `launch_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // `container_id` is not a valid RFC-4122 Version 4 UUID.
  ContainerID badContainerId;
  badContainerId.set_value("not-a-uuid");

  agent::Call::LaunchNestedContainer* launch =
    call.mutable_launch_nested_container();

  launch->mutable_container_id()->CopyFrom(badContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Valid `container_id` but missing `container_id.parent`.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->CopyFrom(parentContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);

  // Not expecting a `container_id.parent.parent`.
  ContainerID grandparentContainerId;
  grandparentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->mutable_parent()
    ->CopyFrom(grandparentContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);
}


TEST(AgentCallValidationTest, WaitNestedContainer)
{
  // Missing `wait_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  agent::Call::WaitNestedContainer* wait =
    call.mutable_wait_nested_container();

  wait->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);

  // Not expecting a `container_id.parent`.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  wait->mutable_container_id()->mutable_parent()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);
}


TEST(AgentCallValidationTest, KillNestedContainer)
{
  // Missing `kill_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::KILL_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  agent::Call::KillNestedContainer* kill =
    call.mutable_kill_nested_container();

  kill->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);

  // Not expecting a `container_id.parent`.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  kill->mutable_container_id()->mutable_parent()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
