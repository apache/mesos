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

#include <string>

#include <mesos/agent/agent.hpp>

#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "slave/validation.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace validation {

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

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER: {
      if (!call.has_launch_nested_container()) {
        return Error("Expecting 'launch_nested_container' to be present");
      }

      const mesos::agent::Call::LaunchNestedContainer& launchNestedContainer =
        call.launch_nested_container();

      // The `ContainerID` must be a RFC-4122 Version 4 UUID
      // in standard string format.
      Try<UUID> uuid = UUID::fromString(
          launchNestedContainer.container_id().value());

      if (uuid.isError()) {
        return Error("'launch_nested_container.container_id.value' must be"
                     " an RFC-4122 version 4 UUID in string format: " +
                     uuid.error());
      }

      if (uuid->version() != UUID::version_random_number_based) {
        return Error("Expected version 4 UUID but was version"
                     " " + stringify(uuid->version()));
      }

      // A single parent `ContainerID` is expected, so that we know
      // which container to place it underneath.
      if (!launchNestedContainer.container_id().has_parent()) {
        return Error("Expecting 'launch_nested_container.container_id.parent'"
                     " to be present");
      } else if (launchNestedContainer.container_id().parent().has_parent()) {
        return Error("Expecting a single parent ContainerID but"
                     " 'launch_nested_container.container_id.parent.parent'"
                     " is set");
      }

      return None();
    }

    case mesos::agent::Call::WAIT_NESTED_CONTAINER:
      if (!call.has_wait_nested_container()) {
        return Error("Expecting 'wait_nested_container' to be present");
      }

      if (call.wait_nested_container().container_id().has_parent()) {
        return Error("Not expecting 'wait_nested_container.container_id.parent'"
                     " to be present");
      }

      return None();

    case mesos::agent::Call::KILL_NESTED_CONTAINER:
      if (!call.has_kill_nested_container()) {
        return Error("Expecting 'kill_nested_container' to be present");
      }

      if (call.kill_nested_container().container_id().has_parent()) {
        return Error("Not expecting 'kill_nested_container.container_id.parent'"
                     " to be present");
      }

      return None();
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

      Try<UUID> uuid = UUID::fromBytes(status.uuid()).get();
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
