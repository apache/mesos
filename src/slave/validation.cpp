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

#include "slave/validation.hpp"

#include <string>

#include <mesos/agent/agent.hpp>

#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "checks/checker.hpp"

#include "common/validation.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace validation {

namespace container {

Option<Error> validateContainerId(const ContainerID& containerId)
{
  const string& id = containerId.value();

  // Check common Mesos ID rules.
  Option<Error> error = common::validation::validateID(id);
  if (error.isSome()) {
    return Error(error->message);
  }

  // Check ContainerID specific rules.
  //
  // Periods are disallowed because our string representation of
  // ContainerID uses periods: <uuid>.<child>.<grandchild>.
  // For example: <uuid>.redis.backup
  //
  // Spaces are disallowed as they can render logs confusing and
  // need escaping on terminals when dealing with paths.
  auto invalidCharacter = [](char c) {
    return  c == '.' || c == ' ';
  };

  if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
    return Error("'ContainerID.value' '" + id + "'"
                 " contains invalid characters");
  }

  // TODO(bmahler): Print the invalid field nicely within the error
  // (e.g. 'parent.parent.parent.value'). For now we only have one
  // level of nesting so it's ok.
  if (containerId.has_parent()) {
    Option<Error> parentError = validateContainerId(containerId.parent());

    if (parentError.isSome()) {
      return Error("'ContainerID.parent' is invalid: " + parentError->message);
    }
  }

  return None();
}

} // namespace container {

namespace agent {
namespace call {

Option<Error> validate(
    const mesos::agent::Call& call,
    const Option<string>& principal)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  switch (call.type()) {
    case mesos::agent::Call::UNKNOWN:
      return None();

    case mesos::agent::Call::GET_HEALTH:
      return None();

    case mesos::agent::Call::GET_FLAGS:
      return None();

    case mesos::agent::Call::GET_VERSION:
      return None();

    case mesos::agent::Call::GET_METRICS:
      if (!call.has_get_metrics()) {
        return Error("Expecting 'get_metrics' to be present");
      }
      return None();

    case mesos::agent::Call::GET_LOGGING_LEVEL:
      return None();

    case mesos::agent::Call::SET_LOGGING_LEVEL:
      if (!call.has_set_logging_level()) {
        return Error("Expecting 'set_logging_level' to be present");
      }
      return None();

    case mesos::agent::Call::LIST_FILES:
      if (!call.has_list_files()) {
        return Error("Expecting 'list_files' to be present");
      }
      return None();

    case mesos::agent::Call::READ_FILE:
      if (!call.has_read_file()) {
        return Error("Expecting 'read_file' to be present");
      }
      return None();

    case mesos::agent::Call::GET_STATE:
      return None();

    case mesos::agent::Call::GET_CONTAINERS:
      return None();

    case mesos::agent::Call::GET_FRAMEWORKS:
      return None();

    case mesos::agent::Call::GET_EXECUTORS:
      return None();

    case mesos::agent::Call::GET_TASKS:
      return None();

    case mesos::agent::Call::GET_AGENT:
      return None();

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER: {
      if (!call.has_launch_nested_container()) {
        return Error("Expecting 'launch_nested_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.launch_nested_container().container_id());

      if (error.isSome()) {
        return Error("'launch_nested_container.container_id' is invalid"
                     ": " + error->message);
      }

      // The parent `ContainerID` is required, so that we know
      // which container to place it underneath.
      if (!call.launch_nested_container().container_id().has_parent()) {
        return Error("Expecting 'launch_nested_container.container_id.parent'"
                     " to be present");
      }

      if (call.launch_nested_container().has_command()) {
        error = common::validation::validateCommandInfo(
            call.launch_nested_container().command());
        if (error.isSome()) {
          return Error("'launch_nested_container.command' is invalid"
                       ": " + error->message);
        }
      }

      return None();
    }

    case mesos::agent::Call::WAIT_NESTED_CONTAINER: {
      if (!call.has_wait_nested_container()) {
        return Error("Expecting 'wait_nested_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.wait_nested_container().container_id());

      if (error.isSome()) {
        return Error("'wait_nested_container.container_id' is invalid"
                     ": " + error->message);
      }

      // Nested containers always have at least one parent.
      if (!call.wait_nested_container().container_id().has_parent()) {
        return Error("Expecting 'wait_nested_container.container_id.parent'"
                     " to be present");
      }

      return None();
    }

    case mesos::agent::Call::KILL_NESTED_CONTAINER: {
      if (!call.has_kill_nested_container()) {
        return Error("Expecting 'kill_nested_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.kill_nested_container().container_id());

      if (error.isSome()) {
        return Error("'kill_nested_container.container_id' is invalid"
                     ": " + error->message);
      }

      // Nested containers always have at least one parent.
      if (!call.kill_nested_container().container_id().has_parent()) {
        return Error("Expecting 'kill_nested_container.container_id.parent'"
                     " to be present");
      }

      return None();
    }

    case mesos::agent::Call::REMOVE_NESTED_CONTAINER: {
      if (!call.has_remove_nested_container()) {
        return Error("Expecting 'remove_nested_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.remove_nested_container().container_id());

      if (error.isSome()) {
        return Error("'remove_nested_container.container_id' is invalid"
                     ": " + error->message);
      }

      // Nested containers always have at least one parent.
      if (!call.remove_nested_container().container_id().has_parent()) {
        return Error("Expecting 'remove_nested_container.container_id.parent'"
                     " to be present");
      }

      return None();
    }

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION: {
      if (!call.has_launch_nested_container_session()) {
        return Error(
            "Expecting 'launch_nested_container_session' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.launch_nested_container_session().container_id());

      if (error.isSome()) {
        return Error("'launch_nested_container_session.container_id' is invalid"
                     ": " + error->message);
      }

      // The parent `ContainerID` is required, so that we know
      // which container to place it underneath.
      if (!call.launch_nested_container_session().container_id().has_parent()) {
        return Error(
            "Expecting 'launch_nested_container_session.container_id.parent'"
            " to be present");
      }

      if (call.launch_nested_container_session().has_command()) {
        error = common::validation::validateCommandInfo(
            call.launch_nested_container_session().command());
        if (error.isSome()) {
          return Error("'launch_nested_container_session.command' is invalid"
                       ": " + error->message);
        }
      }

      return None();
    }

    case mesos::agent::Call::ATTACH_CONTAINER_INPUT: {
      if (!call.has_attach_container_input()) {
        return Error("Expecting 'attach_container_input' to be present");
      }

      if (!call.attach_container_input().has_type()) {
        return Error("Expecting 'attach_container_input.type' to be present");
      }

      switch (call.attach_container_input().type()) {
        case mesos::agent::Call::AttachContainerInput::UNKNOWN:
          return Error("'attach_container_input.type' is unknown");

        case mesos::agent::Call::AttachContainerInput::CONTAINER_ID: {
          Option<Error> error = validation::container::validateContainerId(
              call.attach_container_input().container_id());

          if (error.isSome()) {
            return Error("'attach_container_input.container_id' is invalid"
                ": " + error->message);
          }
        }

        case mesos::agent::Call::AttachContainerInput::PROCESS_IO:
          return None();
      }

      UNREACHABLE();
    }

    case mesos::agent::Call::ATTACH_CONTAINER_OUTPUT: {
      if (!call.has_attach_container_output()) {
        return Error("Expecting 'attach_container_output' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.attach_container_output().container_id());

      if (error.isSome()) {
        return Error("'attach_container_output.container_id' is invalid"
                     ": " + error->message);
      }

      return None();
    }
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace agent {

namespace executor {
namespace call {

Option<Error> validate(const mesos::executor::Call& call)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  // All calls should have executor id set.
  if (!call.has_executor_id()) {
    return Error("Expecting 'executor_id' to be present");
  }

  // All calls should have framework id set.
  if (!call.has_framework_id()) {
    return Error("Expecting 'framework_id' to be present");
  }

  switch (call.type()) {
    case mesos::executor::Call::SUBSCRIBE: {
      if (!call.has_subscribe()) {
        return Error("Expecting 'subscribe' to be present");
      }
      return None();
    }

    case mesos::executor::Call::UPDATE: {
      if (!call.has_update()) {
        return Error("Expecting 'update' to be present");
      }

      const TaskStatus& status = call.update().status();

      if (!status.has_uuid()) {
        return Error("Expecting 'uuid' to be present");
      }

      Try<UUID> uuid = UUID::fromBytes(status.uuid());
      if (uuid.isError()) {
        return uuid.error();
      }

      if (status.has_executor_id() &&
          status.executor_id().value()
          != call.executor_id().value()) {
        return Error("ExecutorID in Call: " +
                     call.executor_id().value() +
                     " does not match ExecutorID in TaskStatus: " +
                     call.update().status().executor_id().value()
                     );
      }

      if (status.source() != TaskStatus::SOURCE_EXECUTOR) {
        return Error("Received Call from executor " +
                     call.executor_id().value() +
                     " of framework " +
                     call.framework_id().value() +
                     " with invalid source, expecting 'SOURCE_EXECUTOR'"
                     );
      }

      if (status.state() == TASK_STAGING) {
        return Error("Received TASK_STAGING from executor " +
                     call.executor_id().value() +
                     " of framework " +
                     call.framework_id().value() +
                     " which is not allowed"
                     );
      }

      // TODO(alexr): Validate `check_status` is present if
      // the corresponding `TaskInfo.check` has been defined.

      if (status.has_check_status()) {
        Option<Error> validate =
          checks::validation::checkStatusInfo(status.check_status());

        if (validate.isSome()) {
          return validate.get();
        }
      }

      return None();
    }

    case mesos::executor::Call::MESSAGE: {
      if (!call.has_message()) {
        return Error("Expecting 'message' to be present");
      }
      return None();
    }

    case mesos::executor::Call::UNKNOWN: {
      return None();
    }
  }
  UNREACHABLE();
}

} // namespace call {
} // namespace executor {
} // namespace validation {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
