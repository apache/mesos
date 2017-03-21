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

#include "common/validation.hpp"

#include "slave/slave.hpp"
#include "slave/validation.hpp"

#include "tests/mesos.hpp"

namespace validation = mesos::internal::slave::validation;

using mesos::internal::common::validation::validateEnvironment;
using mesos::internal::common::validation::validateSecret;

using mesos::internal::slave::Slave;

namespace mesos {
namespace internal {
namespace tests {

TEST(AgentValidationTest, ContainerID)
{
  ContainerID containerId;
  Option<Error> error;

  // No empty IDs.
  containerId.set_value("");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // No slashes.
  containerId.set_value("/");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  containerId.set_value("\\");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // No spaces.
  containerId.set_value("redis backup");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // No periods.
  containerId.set_value("redis.backup");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // Cannot be '.'.
  containerId.set_value(".");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // Cannot be '..'.
  containerId.set_value("..");
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // Valid.
  containerId.set_value("redis");
  error = validation::container::validateContainerId(containerId);
  EXPECT_NONE(error);

  // Valid with invalid parent (empty `ContainerID.value`).
  containerId.set_value("backup");
  containerId.mutable_parent();
  error = validation::container::validateContainerId(containerId);
  EXPECT_SOME(error);

  // Valid with valid parent.
  containerId.set_value("backup");
  containerId.mutable_parent()->set_value("redis");
  error = validation::container::validateContainerId(containerId);
  EXPECT_NONE(error);
}


// Tests that the common validation code for the
// `Secret` message works as expected.
TEST(AgentValidationTest, Secret)
{
  // Test a secret of VALUE type.
  {
    Secret secret;
    secret.set_type(Secret::VALUE);

    Option<Error> error = validateSecret(secret);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Secret of type VALUE must have the 'value' field set",
        error->message);

    secret.mutable_value()->set_data("SECRET_VALUE");
    secret.mutable_reference()->set_name("SECRET_NAME");

    error = validateSecret(secret);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Secret of type VALUE must not have the 'reference' field set",
        error->message);

    // Test the valid case.
    secret.clear_reference();
    error = validateSecret(secret);
    EXPECT_NONE(error);
  }

  // Test a secret of REFERENCE type.
  {
    Secret secret;
    secret.set_type(Secret::REFERENCE);

    Option<Error> error = validateSecret(secret);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Secret of type REFERENCE must have the 'reference' field set",
        error->message);

    secret.mutable_reference()->set_name("SECRET_NAME");
    secret.mutable_value()->set_data("SECRET_VALUE");

    error = validateSecret(secret);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Secret 'SECRET_NAME' of type REFERENCE "
        "must not have the 'value' field set",
        error->message);

    // Test the valid case.
    secret.clear_value();
    error = validateSecret(secret);
    EXPECT_NONE(error);
  }
}


// Tests that the common validation code for the
// `Environment` message works as expected.
TEST(AgentValidationTest, Environment)
{
  // Validate a variable of SECRET type.
  {
    Environment environment;
    Environment::Variable* variable = environment.mutable_variables()->Add();
    variable->set_type(mesos::Environment::Variable::SECRET);
    variable->set_name("ENV_VAR_KEY");

    Option<Error> error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable 'ENV_VAR_KEY' of type "
        "'SECRET' must have a secret set",
        error->message);

    Secret secret;
    secret.set_type(Secret::VALUE);
    secret.mutable_value()->set_data("SECRET_VALUE");
    variable->mutable_secret()->CopyFrom(secret);

    variable->set_value("ENV_VAR_VALUE");

    error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable 'ENV_VAR_KEY' of type 'SECRET' "
        "must not have a value set",
        error->message);

    variable->clear_value();
    char invalid_secret[5] = {'a', 'b', '\0', 'c', 'd'};
    variable->mutable_secret()->mutable_value()->set_data(
        std::string(invalid_secret, 5));

    error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable 'ENV_VAR_KEY' specifies a secret containing "
        "null bytes, which is not allowed in the environment",
        error->message);

    // Test the valid case.
    variable->mutable_secret()->mutable_value()->set_data("SECRET_VALUE");
    error = validateEnvironment(environment);
    EXPECT_NONE(error);
  }

  // Validate a variable of VALUE type.
  {
    // The default type for an environment variable
    // should be VALUE, so we do not set the type here.
    Environment environment;
    Environment::Variable* variable = environment.mutable_variables()->Add();
    variable->set_name("ENV_VAR_KEY");

    Option<Error> error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable 'ENV_VAR_KEY' of type 'VALUE' "
        "must have a value set",
        error->message);

    variable->set_value("ENV_VAR_VALUE");

    Secret secret;
    secret.set_type(Secret::VALUE);
    secret.mutable_value()->set_data("SECRET_VALUE");
    variable->mutable_secret()->CopyFrom(secret);

    error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable 'ENV_VAR_KEY' of type 'VALUE' "
        "must not have a secret set",
        error->message);

    // Test the valid case.
    variable->clear_secret();
    error = validateEnvironment(environment);
    EXPECT_NONE(error);
  }

  // Validate a variable of UNKNOWN type.
  {
    Environment environment;
    Environment::Variable* variable = environment.mutable_variables()->Add();
    variable->set_type(mesos::Environment::Variable::UNKNOWN);
    variable->set_name("ENV_VAR_KEY");
    variable->set_value("ENV_VAR_VALUE");

    Option<Error> error = validateEnvironment(environment);
    EXPECT_SOME(error);
    EXPECT_EQ(
        "Environment variable of type 'UNKNOWN' is not allowed",
        error->message);
  }
}


TEST(AgentCallValidationTest, LaunchNestedContainer)
{
  // Missing `launch_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // `container_id` is not valid.
  ContainerID badContainerId;
  badContainerId.set_value("no spaces allowed");

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

  // Valid `container_id.parent` but invalid `command.environment`. Set
  // an invalid environment variable to check that the common validation
  // code for the command's environment is being executed.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->CopyFrom(parentContainerId);
  launch->mutable_command()->CopyFrom(createCommandInfo("exit 0"));

  Environment::Variable* variable = launch
    ->mutable_command()
    ->mutable_environment()
    ->mutable_variables()
    ->Add();
  variable->set_name("ENV_VAR_KEY");
  variable->set_type(mesos::Environment::Variable::VALUE);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);
  EXPECT_EQ(
      "'launch_nested_container.command' is invalid: Environment variable "
      "'ENV_VAR_KEY' of type 'VALUE' must have a value set",
      error->message);

  // Test the valid case.
  variable->set_value("env_var_value");
  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);

  // Any number of parents is valid.
  ContainerID grandparentContainerId;
  grandparentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->mutable_parent()
    ->CopyFrom(grandparentContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);
}


TEST(AgentCallValidationTest, WaitNestedContainer)
{
  // Missing `wait_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Expecting a `container_id.parent`.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  agent::Call::WaitNestedContainer* wait =
    call.mutable_wait_nested_container();

  wait->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  wait->mutable_container_id()->mutable_parent()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);
}


TEST(AgentCallValidationTest, KillNestedContainer)
{
  // Missing `kill_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::KILL_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Expecting a `container_id.parent`.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  agent::Call::KillNestedContainer* kill =
    call.mutable_kill_nested_container();

  kill->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  kill->mutable_container_id()->mutable_parent()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);
}


TEST(AgentCallValidationTest, RemoveNestedContainer)
{
  // Missing `remove_nested_container`.
  agent::Call call;
  call.set_type(agent::Call::REMOVE_NESTED_CONTAINER);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Expecting a `container_id.parent`.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  agent::Call::RemoveNestedContainer* removeNestedContainer =
    call.mutable_remove_nested_container();

  removeNestedContainer->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Test the valid case.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  removeNestedContainer->mutable_container_id()->mutable_parent()->CopyFrom(
      containerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);
}


TEST(AgentCallValidationTest, LaunchNestedContainerSession)
{
  // Missing `launch_nested_container_session`.
  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  Option<Error> error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // `container_id` is not valid.
  ContainerID badContainerId;
  badContainerId.set_value("no spaces allowed");

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(badContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Valid `container_id` but missing `container_id.parent`.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->CopyFrom(containerId);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);

  // Valid `container_id.parent` but invalid `command.environment`. Set
  // an invalid environment variable to check that the common validation
  // code for the command's environment is being executed.
  ContainerID parentContainerId;
  parentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->CopyFrom(parentContainerId);
  launch->mutable_command()->CopyFrom(createCommandInfo("exit 0"));

  Environment::Variable* variable = launch
    ->mutable_command()
    ->mutable_environment()
    ->mutable_variables()
    ->Add();
  variable->set_name("ENV_VAR_KEY");
  variable->set_type(mesos::Environment::Variable::VALUE);

  error = validation::agent::call::validate(call);
  EXPECT_SOME(error);
  EXPECT_EQ(
      "'launch_nested_container_session.command' is invalid: Environment "
      "variable 'ENV_VAR_KEY' of type 'VALUE' must have a value set",
      error->message);

  // Test the valid case.
  variable->set_value("env_var_value");
  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);

  // Any number of parents is valid.
  ContainerID grandparentContainerId;
  grandparentContainerId.set_value(UUID::random().toString());

  launch->mutable_container_id()->mutable_parent()->mutable_parent()->CopyFrom(
      grandparentContainerId);

  error = validation::agent::call::validate(call);
  EXPECT_NONE(error);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
