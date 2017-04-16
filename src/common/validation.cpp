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

#include "common/validation.hpp"

#include <limits.h>

#include <algorithm>
#include <cctype>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/constants.hpp>

using std::string;

namespace mesos {
namespace internal {
namespace common {
namespace validation {

Option<Error> validateID(const string& id)
{
  if (id.empty()) {
    return Error("ID must not be empty");
  }

  if (id.length() > NAME_MAX) {
    return Error(
        "ID must not be greater than " +
        stringify(NAME_MAX) + " characters");
  }

  // The ID cannot be exactly these special path components.
  if (id == "." || id == "..") {
    return Error("'" + id + "' is disallowed");
  }

  // Rules on invalid characters in the ID:
  // - Control characters are obviously not allowed.
  // - Slashes are disallowed as IDs are likely mapped to directories in Mesos.
  auto invalidCharacter = [](char c) {
    return iscntrl(c) ||
           c == os::POSIX_PATH_SEPARATOR ||
           c == os::WINDOWS_PATH_SEPARATOR;
  };

  if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
    return Error("'" + id + "' contains invalid characters");
  }

  return None();
}


// These IDs are valid as long as they meet the common ID requirements
// enforced by `validateID()` but we define each of them separately to
// be clear which IDs are subject to which rules.
Option<Error> validateTaskID(const TaskID& taskId)
{
  return validateID(taskId.value());
}


Option<Error> validateExecutorID(const ExecutorID& executorId)
{
  return validateID(executorId.value());
}


Option<Error> validateSlaveID(const SlaveID& slaveId)
{
  return validateID(slaveId.value());
}


Option<Error> validateFrameworkID(const FrameworkID& frameworkId)
{
  return validateID(frameworkId.value());
}


Option<Error> validateSecret(const Secret& secret)
{
  switch (secret.type()) {
    case Secret::REFERENCE:
      if (!secret.has_reference()) {
        return Error(
            "Secret of type REFERENCE must have the 'reference' field set");
      }

      if (secret.has_value()) {
        return Error(
            "Secret '" + secret.reference().name() + "' of type REFERENCE "
            "must not have the 'value' field set");
      }
      break;

    case Secret::VALUE:
      if (!secret.has_value()) {
        return Error("Secret of type VALUE must have the 'value' field set");
      }

      if (secret.has_reference()) {
        return Error(
            "Secret of type VALUE must not have the 'reference' field set");
      }
      break;

    case Secret::UNKNOWN:
      break;

    UNREACHABLE();
  }

  return None();
}


Option<Error> validateEnvironment(const Environment& environment)
{
  foreach (const Environment::Variable& variable, environment.variables()) {
    switch (variable.type()) {
      case Environment::Variable::SECRET: {
        if (!variable.has_secret()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'SECRET' must have a secret set");
        }

        if (variable.has_value()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'SECRET' must not have a value set");
        }

        Option<Error> error = validateSecret(variable.secret());
        if (error.isSome()) {
          return Error(
              "Environment variable '" + variable.name() + "' specifies an "
              "invalid secret: " + error->message);
        }

        if (variable.secret().value().data().find('\0') != string::npos) {
            return Error(
                "Environment variable '" + variable.name() + "' specifies a "
                "secret containing null bytes, which is not allowed in the "
                "environment");
        }
        break;
      }

      // NOTE: If new variable types are added in the future and an upgraded
      // client/master sends a new type to an older master/agent, the older
      // master/agent will see VALUE instead of the new type, since VALUE is set
      // as the default type in the protobuf definition.
      case Environment::Variable::VALUE:
        if (!variable.has_value()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'VALUE' must have a value set");
        }

        if (variable.has_secret()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'VALUE' must not have a secret set");
        }
        break;

      case Environment::Variable::UNKNOWN:
          return Error("Environment variable of type 'UNKNOWN' is not allowed");

      UNREACHABLE();
    }
  }

  return None();
}


// TODO(greggomann): Do more than just validate the `Environment`.
Option<Error> validateCommandInfo(const CommandInfo& command)
{
  return validateEnvironment(command.environment());
}

} // namespace validation {
} // namespace common {
} // namespace internal {
} // namespace mesos {
