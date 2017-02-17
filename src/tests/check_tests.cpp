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

#include <mesos/mesos.hpp>

#include "checks/checker.hpp"

#include "tests/mesos.hpp"

namespace mesos {
namespace internal {
namespace tests {

class CheckTest : public MesosTest {};


// This tests ensures `CheckInfo` protobuf is validated correctly.
TEST_F(CheckTest, CheckInfoValidation)
{
  using namespace mesos::internal::checks;

  // Check type must be set to a known value.
  {
    CheckInfo checkInfo;

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("CheckInfo must specify 'type'", validate->message);

    checkInfo.set_type(CheckInfo::UNKNOWN);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("'UNKNOWN' is not a valid check type", validate->message);
  }

  // The associated message for a given type must be set.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::COMMAND);
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'command' to be set for command check",
        validate->message);

    checkInfo.set_type(CheckInfo::HTTP);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'http' to be set for HTTP check",
        validate->message);
  }

  // Command check must specify an actual command in `command.command.value`.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::COMMAND);
    checkInfo.mutable_command()->CopyFrom(CheckInfo::Command());
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("Command check must contain 'shell command'", validate->message);

    checkInfo.mutable_command()->mutable_command()->CopyFrom(CommandInfo());
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("Command check must contain 'shell command'", validate->message);
  }

  // Command check must specify a command with a valid environment.
  // Environment variable's `value` field must be set in this case.
  {
    CheckInfo checkInfo;
    checkInfo.set_type(CheckInfo::COMMAND);
    checkInfo.mutable_command()->CopyFrom(CheckInfo::Command());
    checkInfo.mutable_command()->mutable_command()->CopyFrom(
        createCommandInfo("exit 0"));

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);

    Environment::Variable* variable =
      checkInfo.mutable_command()->mutable_command()->mutable_environment()
          ->mutable_variables()->Add();
    variable->set_name("ENV_VAR_KEY");

    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
  }

  // HTTP check may specify a path starting with '/'.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::HTTP);
    checkInfo.mutable_http()->set_port(8080);

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);

    checkInfo.mutable_http()->set_path("healthz");
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "The path 'healthz' of HTTP  check must start with '/'",
        validate->message);
  }

  // Check's duration parameters must be non-negative.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::HTTP);
    checkInfo.mutable_http()->set_port(8080);

    checkInfo.set_delay_seconds(-1.0);
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'delay_seconds' to be non-negative",
        validate->message);

    checkInfo.set_delay_seconds(0.0);
    checkInfo.set_interval_seconds(-1.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'interval_seconds' to be non-negative",
        validate->message);

    checkInfo.set_interval_seconds(0.0);
    checkInfo.set_timeout_seconds(-1.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'timeout_seconds' to be non-negative",
        validate->message);

    checkInfo.set_timeout_seconds(0.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);
  }
}


// This tests ensures `CheckStatusInfo` protobuf is validated correctly.
TEST_F(CheckTest, CheckStatusInfoValidation)
{
  using namespace mesos::internal::checks;

  // Check status type must be set to a known value.
  {
    CheckStatusInfo checkStatusInfo;

    Option<Error> validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("CheckStatusInfo must specify 'type'", validate->message);

    checkStatusInfo.set_type(CheckInfo::UNKNOWN);
    validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "'UNKNOWN' is not a valid check's status type",
        validate->message);
  }

  // The associated message for a given type must be set.
  {
    CheckStatusInfo checkStatusInfo;

    checkStatusInfo.set_type(CheckInfo::COMMAND);
    Option<Error> validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'command' to be set for command check's status",
        validate->message);

    checkStatusInfo.set_type(CheckInfo::HTTP);
    validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'http' to be set for HTTP check's status",
        validate->message);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
