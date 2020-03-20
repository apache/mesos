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

#include "slave/constants.hpp"
#include "slave/validation.hpp"

#include <string>

#include <mesos/resources.hpp>

#include <mesos/agent/agent.hpp>

#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include "common/resources_utils.hpp"
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
  // Valid the container id length
  if (id.length() > MAX_CONTAINER_ID_LENGTH) {
    return Error("'ContainerID.value' '" + id + "' exceeds the maximum"
                 " length (" + stringify(MAX_CONTAINER_ID_LENGTH) + ")");
  }

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

    case mesos::agent::Call::GET_OPERATIONS:
      return None();

    case mesos::agent::Call::GET_TASKS:
      return None();

    case mesos::agent::Call::GET_AGENT:
      return None();

    case mesos::agent::Call::GET_RESOURCE_PROVIDERS:
      return None();

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER: {
      if (!call.has_launch_nested_container()) {
        return Error("Expecting 'launch_nested_container' to be present");
      }

      const mesos::agent::Call::LaunchNestedContainer& launch =
        call.launch_nested_container();

      Option<Error> error = validation::container::validateContainerId(
          launch.container_id());

      if (error.isSome()) {
        return Error("'launch_nested_container.container_id' is invalid"
                     ": " + error->message);
      }

      // The parent `ContainerID` is required, so that we know
      // which container to place it underneath.
      if (!launch.container_id().has_parent()) {
        return Error("Expecting 'launch_nested_container.container_id.parent'"
                     " to be present");
      }

      if (launch.has_command()) {
        error = common::validation::validateCommandInfo(launch.command());
        if (error.isSome()) {
          return Error("'launch_nested_container.command' is invalid"
                       ": " + error->message);
        }
      }

      if (launch.has_container()) {
        error = common::validation::validateContainerInfo(launch.container());
        if (error.isSome()) {
          return Error("'launch_nested_container.container' is invalid"
                       ": " + error->message);
        }

        if (launch.container().has_linux_info() &&
            launch.container().linux_info().has_share_cgroups() &&
            !launch.container().linux_info().share_cgroups() &&
            launch.container_id().has_parent() &&
            launch.container_id().parent().has_parent()) {
            return Error(
                "'launch_nested_container' is invalid: containers nested at "
                "the second level or greater cannot set 'share_cgroups' to "
                "'false'");
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

      const mesos::agent::Call::LaunchNestedContainerSession& launch =
        call.launch_nested_container_session();

      Option<Error> error = validation::container::validateContainerId(
          launch.container_id());

      if (error.isSome()) {
        return Error("'launch_nested_container_session.container_id' is invalid"
                     ": " + error->message);
      }

      // The parent `ContainerID` is required, so that we know
      // which container to place it underneath.
      if (!launch.container_id().has_parent()) {
        return Error(
            "Expecting 'launch_nested_container_session.container_id.parent'"
            " to be present");
      }

      if (launch.has_command()) {
        error = common::validation::validateCommandInfo(launch.command());
        if (error.isSome()) {
          return Error("'launch_nested_container_session.command' is invalid"
                       ": " + error->message);
        }
      }

      if (launch.has_container()) {
        error = common::validation::validateContainerInfo(launch.container());
        if (error.isSome()) {
          return Error("'launch_nested_container_session.container' is invalid"
                       ": " + error->message);
        }

        if (launch.container().has_linux_info() &&
            !launch.container().linux_info().share_cgroups()) {
          return Error(
              "'launch_nested_container_session.container.linux_info' is "
              "invalid: 'share_cgroups' cannot be set to 'false' for nested "
              "container sessions");
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

    case mesos::agent::Call::LAUNCH_CONTAINER: {
      if (!call.has_launch_container()) {
        return Error("Expecting 'launch_container' to be present");
      }

      const mesos::agent::Call::LaunchContainer& launch =
        call.launch_container();

      Option<Error> error = validation::container::validateContainerId(
          launch.container_id());

      if (error.isSome()) {
        return Error(
            "'launch_container.container_id' is invalid: " + error->message);
      }

      // General resource validation first.
      error = Resources::validate(launch.resources());
      if (error.isSome()) {
        return Error("Invalid resources: " + error->message);
      }

      error = common::validation::validateGpus(launch.resources());
      if (error.isSome()) {
        return Error("Invalid GPU resources: " + error->message);
      }

      // Because standalone containers are launched outside of the master's
      // offer cycle, some resource types or fields may not be specified.
      if (!launch.container_id().has_parent()) {
        foreach (Resource resource, launch.resources()) {
          // Upgrade the resources (in place) to simplify validation.
          upgradeResource(&resource);

          // Standalone containers may only use unreserved resources.
          // There is no accounting in the master for resources consumed
          // by standalone containers, so allowing reserved resources would
          // only increase code complexity with no change in behavior.
          if (Resources::isReserved(resource)) {
            return Error("'launch_container.resources' must be unreserved");
          }

          // NOTE: The master normally requires all volumes be persistent,
          // and that all persistent volumes belong to a role. Standalone
          // containers therefore cannot use persistent volumes.
          if (Resources::isPersistentVolume(resource)) {
            return Error(
                "'launch_container.resources' may not use persistent volumes");
          }

          // Standalone containers are expected to occupy resources *not*
          // advertised by the agent and hence do not need to worry about
          // being preempted or throttled.
          if (Resources::isRevocable(resource)) {
            return Error("'launch_container.resources' must be non-revocable");
          }
        }
      }

      if (launch.has_command()) {
        error = common::validation::validateCommandInfo(launch.command());
        if (error.isSome()) {
          return Error(
              "'launch_container.command' is invalid: " + error->message);
        }
      }

      if (launch.has_container()) {
        error = common::validation::validateContainerInfo(launch.container());
        if (error.isSome()) {
          return Error(
              "'launch_container.container' is invalid: " + error->message);
        }
      }

      bool shareCgroups =
        (launch.has_container() &&
         launch.container().has_linux_info() &&
         launch.container().linux_info().has_share_cgroups()) ?
           launch.container().linux_info().share_cgroups() :
           true;

      bool twiceNested =
        (launch.container_id().has_parent() &&
         launch.container_id().parent().has_parent());

      if (twiceNested && !launch.resources().empty()) {
        return Error(
            "'launch_container' is invalid: containers nested at the "
            "second level or greater cannot specify resources");
      }

      if (twiceNested && !launch.limits().empty()) {
        return Error(
            "'launch_container' is invalid: containers nested at the "
            "second level or greater cannot specify resource limits");
      }

      if (twiceNested && !shareCgroups) {
        return Error(
            "'launch_container' is invalid: containers nested at the "
            "second level or greater cannot set 'share_cgroups' to "
            "'false'");
      }

      if (!launch.container_id().has_parent() &&
          launch.container().linux_info().has_share_cgroups() &&
          shareCgroups) {
        return Error(
            "'launch_container' is invalid: containers without a parent "
            "cannot set 'share_cgroups' to 'true'");
      }

      return None();
    }

    case mesos::agent::Call::WAIT_CONTAINER: {
      if (!call.has_wait_container()) {
        return Error("Expecting 'wait_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.wait_container().container_id());

      if (error.isSome()) {
        return Error("'wait_container.container_id' is invalid"
                     ": " + error->message);
      }

      return None();
    }

    case mesos::agent::Call::KILL_CONTAINER: {
      if (!call.has_kill_container()) {
        return Error("Expecting 'kill_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.kill_container().container_id());

      if (error.isSome()) {
        return Error("'kill_container.container_id' is invalid"
                     ": " + error->message);
      }

      return None();
    }

    case mesos::agent::Call::REMOVE_CONTAINER: {
      if (!call.has_remove_container()) {
        return Error("Expecting 'remove_container' to be present");
      }

      Option<Error> error = validation::container::validateContainerId(
          call.remove_container().container_id());

      if (error.isSome()) {
        return Error("'remove_container.container_id' is invalid"
                     ": " + error->message);
      }

      return None();
    }

    case mesos::agent::Call::ADD_RESOURCE_PROVIDER_CONFIG: {
      if (!call.has_add_resource_provider_config()) {
        return Error(
            "Expecting 'add_resource_provider_config' to be present");
      }

      if (call.add_resource_provider_config().info().has_id()) {
        return Error(
            "Expecting 'add_resource_provider_config.info.id' to be unset");
      }

      return None();
    }

    case mesos::agent::Call::UPDATE_RESOURCE_PROVIDER_CONFIG: {
      if (!call.has_update_resource_provider_config()) {
        return Error(
            "Expecting 'update_resource_provider_config' to be present");
      }

      if (call.update_resource_provider_config().info().has_id()) {
        return Error(
            "Expecting 'update_resource_provider_config.info.id' to be unset");
      }

      return None();
    }

    case mesos::agent::Call::REMOVE_RESOURCE_PROVIDER_CONFIG: {
      if (!call.has_remove_resource_provider_config()) {
        return Error(
            "Expecting 'remove_resource_provider_config' to be present");
      }

      return None();
    }

    case mesos::agent::Call::MARK_RESOURCE_PROVIDER_GONE: {
      if (!call.has_mark_resource_provider_gone()) {
        return Error("Expecting 'mark_resource_provider_gone' to be present");
      }

      return None();
    }

    case mesos::agent::Call::PRUNE_IMAGES: {
      return None();
    }
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace agent {
} // namespace validation {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
