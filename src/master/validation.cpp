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

#include "master/validation.hpp"

#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/roles.hpp>
#include <mesos/type_utils.hpp>

#include <process/authenticator.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/stringify.hpp>

#include "checks/checker.hpp"
#include "checks/health_checker.hpp"

#include "common/protobuf_utils.hpp"
#include "common/validation.hpp"

#include "master/master.hpp"

using process::http::authentication::Principal;

using std::set;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace internal {
namespace master {
namespace validation {
namespace master {
namespace call {

Option<Error> validate(
    const mesos::master::Call& call,
    const Option<Principal>& principal)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  switch (call.type()) {
    case mesos::master::Call::UNKNOWN:
      return None();

    case mesos::master::Call::GET_HEALTH:
      return None();

    case mesos::master::Call::GET_FLAGS:
      return None();

    case mesos::master::Call::GET_VERSION:
      return None();

    case mesos::master::Call::GET_METRICS:
      if (!call.has_get_metrics()) {
        return Error("Expecting 'get_metrics' to be present");
      }
      return None();

    case mesos::master::Call::GET_LOGGING_LEVEL:
      return None();

    case mesos::master::Call::SET_LOGGING_LEVEL:
      if (!call.has_set_logging_level()) {
        return Error("Expecting 'set_logging_level' to be present");
      }
      return None();

    case mesos::master::Call::LIST_FILES:
      if (!call.has_list_files()) {
        return Error("Expecting 'list_files' to be present");
      }
      return None();

    case mesos::master::Call::READ_FILE:
      if (!call.has_read_file()) {
        return Error("Expecting 'read_file' to be present");
      }
      return None();

    case mesos::master::Call::GET_STATE:
      return None();

    case mesos::master::Call::GET_AGENTS:
      return None();

    case mesos::master::Call::GET_FRAMEWORKS:
      return None();

    case mesos::master::Call::GET_EXECUTORS:
      return None();

    case mesos::master::Call::GET_TASKS:
      return None();

    case mesos::master::Call::GET_ROLES:
      return None();

    case mesos::master::Call::GET_WEIGHTS:
      return None();

    case mesos::master::Call::UPDATE_WEIGHTS:
      if (!call.has_update_weights()) {
        return Error("Expecting 'update_weights' to be present");
      }
      return None();

    case mesos::master::Call::GET_MASTER:
      return None();

    case mesos::master::Call::SUBSCRIBE:
      return None();

    case mesos::master::Call::RESERVE_RESOURCES: {
      if (!call.has_reserve_resources()) {
        return Error("Expecting 'reserve_resources' to be present");
      }

      Option<Error> error =
        Resources::validate(call.reserve_resources().resources());

      if (error.isSome()) {
        return error;
      }

      return None();
    }

    case mesos::master::Call::UNRESERVE_RESOURCES: {
      if (!call.has_unreserve_resources()) {
        return Error("Expecting 'unreserve_resources' to be present");
      }

      Option<Error> error =
        Resources::validate(call.unreserve_resources().resources());

      if (error.isSome()) {
        return error;
      }

      return None();
    }

    case mesos::master::Call::CREATE_VOLUMES:
      if (!call.has_create_volumes()) {
        return Error("Expecting 'create_volumes' to be present");
      }
      return None();

    case mesos::master::Call::DESTROY_VOLUMES:
      if (!call.has_destroy_volumes()) {
        return Error("Expecting 'destroy_volumes' to be present");
      }
      return None();

    case mesos::master::Call::GET_MAINTENANCE_STATUS:
      return None();

    case mesos::master::Call::GET_MAINTENANCE_SCHEDULE:
      return None();

    case mesos::master::Call::UPDATE_MAINTENANCE_SCHEDULE:
      if (!call.has_update_maintenance_schedule()) {
        return Error("Expecting 'update_maintenance_schedule' to be present");
      }
      return None();

    case mesos::master::Call::START_MAINTENANCE:
      if (!call.has_start_maintenance()) {
        return Error("Expecting 'start_maintenance' to be present");
      }
      return None();

    case mesos::master::Call::STOP_MAINTENANCE:
      if (!call.has_stop_maintenance()) {
        return Error("Expecting 'stop_maintenance' to be present");
      }
      return None();

    case mesos::master::Call::GET_QUOTA:
      return None();

    case mesos::master::Call::SET_QUOTA:
      if (!call.has_set_quota()) {
        return Error("Expecting 'set_quota' to be present");
      }
      return None();

    case mesos::master::Call::REMOVE_QUOTA:
      if (!call.has_remove_quota()) {
        return Error("Expecting 'remove_quota' to be present");
      }
      return None();
  }

  UNREACHABLE();
}

} // namespace call {

namespace message {

static Option<Error> validateSlaveInfo(const SlaveInfo& slaveInfo)
{
  if (slaveInfo.has_id()) {
    Option<Error> error = common::validation::validateSlaveID(slaveInfo.id());
    if (error.isSome()) {
      return error.get();
    }
  }

  Option<Error> error = Resources::validate(slaveInfo.resources());
  if (error.isSome()) {
    return error.get();
  }

  return None();
}


Option<Error> registerSlave(
    const SlaveInfo& slaveInfo,
    const vector<Resource>& checkpointedResources)
{
  Option<Error> error = validateSlaveInfo(slaveInfo);
  if (error.isSome()) {
    return error.get();
  }

  if (!checkpointedResources.empty()) {
    if (!slaveInfo.has_checkpoint() || !slaveInfo.checkpoint()) {
      return Error(
          "Checkpointed resources provided when checkpointing is not enabled");
    }
  }

  foreach (const Resource& resource, checkpointedResources) {
    error = Resources::validate(resource);
    if (error.isSome()) {
      return error.get();
    }
  }

  return None();
}


Option<Error> reregisterSlave(
    const SlaveInfo& slaveInfo,
    const vector<Task>& tasks,
    const vector<Resource>& resources,
    const vector<ExecutorInfo>& executorInfos,
    const vector<FrameworkInfo>& frameworkInfos)
{
  hashset<FrameworkID> frameworkIDs;
  hashset<ExecutorID> executorIDs;

  Option<Error> error = validateSlaveInfo(slaveInfo);
  if (error.isSome()) {
    return error.get();
  }

  foreach (const Resource& resource, resources) {
    Option<Error> error = Resources::validate(resource);
    if (error.isSome()) {
      return error.get();
    }
  }

  foreach (const FrameworkInfo& framework, frameworkInfos) {
    Option<Error> error = validation::framework::validate(framework);
    if (error.isSome()) {
      return error.get();
    }

    if (frameworkIDs.contains(framework.id())) {
      return Error("Framework has a duplicate FrameworkID: '" +
                  stringify(framework.id()) + "'");
    }

    frameworkIDs.insert(framework.id());
  }

  foreach (const ExecutorInfo& executor, executorInfos) {
    Option<Error> error = validation::executor::validate(executor);
    if (error.isSome()) {
      return error.get();
    }

    // We don't use internal::validateResources() here because
    // that includes the validateAllocatedToSingleRole() check,
    // which is not valid for agent re-registration.
    error = Resources::validate(executor.resources());
    if (error.isSome()) {
      return error.get();
    }

    if (!frameworkIDs.contains(executor.framework_id())) {
      return Error("Executor has an invalid FrameworkID '" +
                   stringify(executor.framework_id()) + "'");
    }

    if (executor.has_executor_id()) {
      if (executorIDs.contains(executor.executor_id())) {
        return Error("Executor has a duplicate ExecutorID '" +
                     stringify(executor.executor_id()) + "'");
      }

      executorIDs.insert(executor.executor_id());
    }
  }

  foreach (const Task& task, tasks) {
    Option<Error> error = common::validation::validateTaskID(task.task_id());
    if (error.isSome()) {
      return Error("Task has an invalid TaskID: " + error->message);
    }

    if (task.slave_id() != slaveInfo.id()) {
      return Error("Task has an invalid SlaveID '" +
                   stringify(task.slave_id()) + "'");
    }

    if (!frameworkIDs.contains(task.framework_id())) {
      return Error("Task has an invalid FrameworkID '" +
                   stringify(task.framework_id()) + "'");
    }

    // Command Executors don't send the executor ID in the task because it
    // is generated on the agent (see Slave::doReliableRegistration). Only
    // running tasks ought to have executors.
    if (task.has_executor_id() && task.state() == TASK_RUNNING) {
      if (!executorIDs.contains(task.executor_id())) {
        return Error("Task has an invalid ExecutorID '" +
                     stringify(task.executor_id()) + "'");
      }
    }

    error = resource::validate(task.resources());
    if (error.isSome()) {
      return Error("Task uses invalid resources: " + error->message);
    }
  }

  return None();
}

} // namespace message {
} // namespace master {


namespace framework {
namespace internal {

Option<Error> validateRoles(const FrameworkInfo& frameworkInfo)
{
  bool multiRole = protobuf::frameworkHasCapability(
      frameworkInfo,
      FrameworkInfo::Capability::MULTI_ROLE);

  // Ensure that the right fields are used.
  if (multiRole) {
    if (frameworkInfo.has_role()) {
      return Error("'FrameworkInfo.role' must not be set when the"
                   " framework is MULTI_ROLE capable");
     }
  } else {
    if (frameworkInfo.roles_size() > 0) {
      return Error("'FrameworkInfo.roles' must not be set when the"
                   " framework is not MULTI_ROLE capable");
    }
  }

  // Check for duplicate entries.
  //
  // TODO(bmahler): Use a generic duplicate check function.
  if (multiRole) {
    const hashset<string> duplicateRoles = [&]() {
      hashset<string> roles;
      hashset<string> duplicates;

      foreach (const string& role, frameworkInfo.roles()) {
        if (roles.contains(role)) {
          duplicates.insert(role);
        } else {
          roles.insert(role);
        }
      }

      return duplicates;
    }();

    if (!duplicateRoles.empty()) {
      return Error("'FrameworkInfo.roles' contains duplicate items: " +
                   stringify(duplicateRoles));
     }
  }

  // Validate the role(s).
  if (multiRole) {
    foreach (const string& role, frameworkInfo.roles()) {
      Option<Error> error = roles::validate(role);
      if (error.isSome()) {
        return Error("'FrameworkInfo.roles' contains invalid role: " +
                     error->message);
      }
    }
  } else {
    Option<Error> error = roles::validate(frameworkInfo.role());
    if (error.isSome()) {
      return Error("'FrameworkInfo.role' is not a valid role: " +
                   error->message);
    }
  }

  return None();
}

} // namespace internal {

Option<Error> validate(const mesos::FrameworkInfo& frameworkInfo)
{
  return internal::validateRoles(frameworkInfo);
}

} // namespace framework {


namespace scheduler {
namespace call {

Option<Error> validate(
    const mesos::scheduler::Call& call,
    const Option<Principal>& principal)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  if (call.type() == mesos::scheduler::Call::SUBSCRIBE) {
    if (!call.has_subscribe()) {
      return Error("Expecting 'subscribe' to be present");
    }

    const FrameworkInfo& frameworkInfo = call.subscribe().framework_info();

    if (frameworkInfo.id() != call.framework_id()) {
      return Error("'framework_id' differs from 'subscribe.framework_info.id'");
    }

    if (principal.isSome() &&
        frameworkInfo.has_principal() &&
        principal != frameworkInfo.principal()) {
      // We assume that `principal->value.isSome()` is true. The master's HTTP
      // handlers enforce this constraint, and V0 authenticators will only
      // return principals of that form.
      CHECK_SOME(principal->value);

      return Error(
          "Authenticated principal '" + stringify(principal.get()) +
          "' does not match principal '" + frameworkInfo.principal() +
          "' set in `FrameworkInfo`");
    }

    return None();
  }

  // All calls except SUBSCRIBE should have framework id set.
  if (!call.has_framework_id()) {
    return Error("Expecting 'framework_id' to be present");
  }

  switch (call.type()) {
    case mesos::scheduler::Call::SUBSCRIBE:
      // SUBSCRIBE call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";

    case mesos::scheduler::Call::TEARDOWN:
      return None();

    case mesos::scheduler::Call::ACCEPT:
      if (!call.has_accept()) {
        return Error("Expecting 'accept' to be present");
      }
      return None();

    case mesos::scheduler::Call::DECLINE:
      if (!call.has_decline()) {
        return Error("Expecting 'decline' to be present");
      }
      return None();

    case mesos::scheduler::Call::ACCEPT_INVERSE_OFFERS:
      if (!call.has_accept_inverse_offers()) {
        return Error("Expecting 'accept_inverse_offers' to be present");
      }
      return None();

    case mesos::scheduler::Call::DECLINE_INVERSE_OFFERS:
      if (!call.has_decline_inverse_offers()) {
        return Error("Expecting 'decline_inverse_offers' to be present");
      }
      return None();

    case mesos::scheduler::Call::REVIVE:
      return None();

    case mesos::scheduler::Call::SUPPRESS:
      return None();

    case mesos::scheduler::Call::KILL:
      if (!call.has_kill()) {
        return Error("Expecting 'kill' to be present");
      }
      return None();

    case mesos::scheduler::Call::SHUTDOWN:
      if (!call.has_shutdown()) {
        return Error("Expecting 'shutdown' to be present");
      }
      return None();

    case mesos::scheduler::Call::ACKNOWLEDGE: {
      if (!call.has_acknowledge()) {
        return Error("Expecting 'acknowledge' to be present");
      }

      Try<UUID> uuid = UUID::fromBytes(call.acknowledge().uuid());
      if (uuid.isError()) {
        return uuid.error();
      }
      return None();
    }

    case mesos::scheduler::Call::RECONCILE:
      if (!call.has_reconcile()) {
        return Error("Expecting 'reconcile' to be present");
      }
      return None();

    case mesos::scheduler::Call::MESSAGE:
      if (!call.has_message()) {
        return Error("Expecting 'message' to be present");
      }
      return None();

    case mesos::scheduler::Call::REQUEST:
      if (!call.has_request()) {
        return Error("Expecting 'request' to be present");
      }
      return None();

    case mesos::scheduler::Call::UNKNOWN:
      return None();
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace scheduler {


namespace resource {

// Validates that the `gpus` resource is not fractional.
// We rely on scalar resources only having 3 digits of precision.
Option<Error> validateGpus(const RepeatedPtrField<Resource>& resources)
{
  double gpus = Resources(resources).gpus().getOrElse(0.0);
  if (static_cast<long long>(gpus * 1000.0) % 1000 != 0) {
    return Error("The 'gpus' resource must be an unsigned integer");
  }

  return None();
}


// Validates the ReservationInfos specified in the given resources (if
// exist). Returns error if any ReservationInfo is found invalid or
// unsupported.
Option<Error> validateDynamicReservationInfo(
    const RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    if (!Resources::isDynamicallyReserved(resource)) {
      continue;
    }

    if (Resources::isRevocable(resource)) {
      return Error(
          "Dynamically reserved resource " + stringify(resource) +
          " cannot be created from revocable resources");
    }
  }

  return None();
}


// Validates the DiskInfos specified in the given resources (if
// exist). Returns error if any DiskInfo is found invalid or
// unsupported.
Option<Error> validateDiskInfo(const RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    if (!resource.has_disk()) {
      continue;
    }

    if (resource.disk().has_persistence()) {
      if (Resources::isRevocable(resource)) {
        return Error(
            "Persistent volumes cannot be created from revocable resources");
      }
      if (Resources::isUnreserved(resource)) {
        return Error(
            "Persistent volumes cannot be created from unreserved resources");
      }
      if (!resource.disk().has_volume()) {
        return Error("Expecting 'volume' to be set for persistent volume");
      }
      if (resource.disk().volume().has_host_path()) {
        return Error("Expecting 'host_path' to be unset for persistent volume");
      }

      // Ensure persistence ID meets common ID requirements.
      Option<Error> error =
        common::validation::validateID(resource.disk().persistence().id());
      if (error.isSome()) {
        return Error("Invalid persistence ID for persistent volume: " +
                     error->message);
      }
    } else if (resource.disk().has_volume()) {
      return Error("Non-persistent volume not supported");
    } else if (!resource.disk().has_source()) {
      return Error("DiskInfo is set but empty");
    }
  }

  return None();
}


// Validates the uniqueness of the persistence IDs used in the given
// resources. They need to be unique per role on each slave.
Option<Error> validateUniquePersistenceID(
    const Resources& resources)
{
  hashmap<string, hashset<string>> persistenceIds;

  // Check duplicated persistence ID within the given resources.
  Resources volumes = resources.persistentVolumes();

  foreach (const Resource& volume, volumes) {
    const string& role = volume.role();
    const string& id = volume.disk().persistence().id();

    if (persistenceIds.contains(role) &&
        persistenceIds[role].contains(id)) {
      return Error("Persistence ID '" + id + "' is not unique");
    }

    persistenceIds[role].insert(id);
  }

  return None();
}


// Validates that revocable and non-revocable
// resources of the same name do not exist.
//
// TODO(vinod): Is this the right place to do this?
Option<Error> validateRevocableAndNonRevocableResources(
    const Resources& _resources)
{
  foreach (const string& name, _resources.names()) {
    Resources resources = _resources.get(name);
    if (!resources.revocable().empty() && resources != resources.revocable()) {
      return Error("Cannot use both revocable and non-revocable '" + name +
                   "' at the same time");
    }
  }

  return None();
}


// Validates that all the given resources are persistent volumes.
Option<Error> validatePersistentVolume(
    const RepeatedPtrField<Resource>& volumes)
{
  foreach (const Resource& volume, volumes) {
    if (!volume.has_disk()) {
      return Error("Resource " + stringify(volume) + " does not have DiskInfo");
    } else if (!volume.disk().has_persistence()) {
      return Error("'persistence' is not set in DiskInfo");
    } else if (!volume.disk().has_volume()) {
      return Error("Expecting 'volume' to be set for persistent volume");
    } else if (volume.disk().volume().mode() == Volume::RO) {
      return Error("Read-only persistent volume not supported");
    }
  }

  return None();
}


// Validates that all the given resources are allocated to same role.
Option<Error> validateAllocatedToSingleRole(const Resources& resources)
{
  Option<string> role;

  foreach (const Resource& resource, resources) {
    // Note that the master normalizes `Offer::Operation` resources
    // to have allocation info set, so we can validate it here.
    if (!resource.allocation_info().has_role()) {
      return Error("The resources are not allocated to a role");
    }

    string _role = resource.allocation_info().role();

    if (role.isNone()) {
      role = _role;
      continue;
    }

    if (_role != role.get()) {
      return Error("The resources have multiple allocation roles"
                   " ('" + _role + "' and '" + role.get() + "')"
                   " but only one allocation role is allowed");
    }
  }

  return None();
}


Option<Error> validate(const RepeatedPtrField<Resource>& resources)
{
  Option<Error> error = Resources::validate(resources);
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = validateGpus(resources);
  if (error.isSome()) {
    return Error("Invalid 'gpus' resource: " + error.get().message);
  }

  error = validateDiskInfo(resources);
  if (error.isSome()) {
    return Error("Invalid DiskInfo: " + error.get().message);
  }

  error = validateDynamicReservationInfo(resources);
  if (error.isSome()) {
    return Error("Invalid ReservationInfo: " + error.get().message);
  }

  return None();
}

} // namespace resource {


namespace executor {
namespace internal {

Option<Error> validateExecutorID(const ExecutorInfo& executor)
{
  // Delegate to the common ExecutorID validation. Here we wrap
  // around it to be consistent with other executor validators.
  return common::validation::validateExecutorID(executor.executor_id());
}


Option<Error> validateType(const ExecutorInfo& executor)
{
  switch (executor.type()) {
    case ExecutorInfo::DEFAULT:
      if (executor.has_command()) {
        return Error(
            "'ExecutorInfo.command' must not be set for 'DEFAULT' executor");
      }

      if (executor.has_container()) {
        if (executor.container().type() != ContainerInfo::MESOS) {
          return Error(
              "'ExecutorInfo.container.type' must be 'MESOS' for "
              "'DEFAULT' executor");
        }

        if (executor.container().mesos().has_image()) {
          return Error(
              "'ExecutorInfo.container.mesos.image' must not be set for "
              "'DEFAULT' executor");
        }
      }
      break;

    case ExecutorInfo::CUSTOM:
      if (!executor.has_command()) {
        return Error(
            "'ExecutorInfo.command' must be set for 'CUSTOM' executor");
      }
      break;

    case ExecutorInfo::UNKNOWN:
      // This could happen if a new executor type is introduced in the
      // protos but the  master doesn't know about it yet (e.g., new
      // scheduler launches new type of executor on an old master).
      return None();
  }

  return None();
}


Option<Error> validateCompatibleExecutorInfo(
    const ExecutorInfo& executor,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  const ExecutorID& executorId = executor.executor_id();
  Option<ExecutorInfo> executorInfo = None();

  if (slave->hasExecutor(framework->id(), executorId)) {
    executorInfo =
      slave->executors.at(framework->id()).at(executorId);
  }

  if (executorInfo.isSome() && !(executor == executorInfo.get())) {
    return Error(
        "ExecutorInfo is not compatible with existing ExecutorInfo"
        " with same ExecutorID).\n"
        "------------------------------------------------------------\n"
        "Existing ExecutorInfo:\n" +
        stringify(executorInfo.get()) + "\n"
        "------------------------------------------------------------\n"
        "ExecutorInfo:\n" +
        stringify(executor) + "\n"
        "------------------------------------------------------------\n");
  }

  return None();
}


Option<Error> validateFrameworkID(
    const ExecutorInfo& executor,
    Framework* framework)
{
  CHECK_NOTNULL(framework);

  // The master fills in `ExecutorInfo.framework_id` for
  // executors used in Launch operations.
  if (!executor.has_framework_id()) {
    return Error("'ExecutorInfo.framework_id' must be set");
  }

  if (executor.framework_id() != framework->id()) {
    return Error(
        "ExecutorInfo has an invalid FrameworkID"
        " (Actual: " + stringify(executor.framework_id()) +
        " vs Expected: " + stringify(framework->id()) + ")");
  }

  return None();
}


Option<Error> validateShutdownGracePeriod(const ExecutorInfo& executor)
{
  // Make sure provided duration is non-negative.
  if (executor.has_shutdown_grace_period() &&
      Nanoseconds(executor.shutdown_grace_period().nanoseconds()) <
        Duration::zero()) {
    return Error(
        "ExecutorInfo's 'shutdown_grace_period' must be non-negative");
  }

  return None();
}


Option<Error> validateResources(const ExecutorInfo& executor)
{
  Option<Error> error = resource::validate(executor.resources());
  if (error.isSome()) {
    return Error("Executor uses invalid resources: " + error->message);
  }

  const Resources& resources = executor.resources();

  error = resource::validateUniquePersistenceID(resources);
  if (error.isSome()) {
    return Error(
        "Executor uses duplicate persistence ID: " + error->message);
  }

  error = resource::validateAllocatedToSingleRole(resources);
  if (error.isSome()) {
    return Error("Invalid executor resources: " + error->message);
  }

  error = resource::validateRevocableAndNonRevocableResources(resources);
  if (error.isSome()) {
    return Error("Executor mixes revocable and non-revocable resources: " +
                 error->message);
  }

  return None();
}


// Validates the `CommandInfo` contained within an `ExecutorInfo`.
Option<Error> validateCommandInfo(const ExecutorInfo& executor)
{
  if (executor.has_command()) {
    Option<Error> error =
      common::validation::validateCommandInfo(executor.command());
    if (error.isSome()) {
      return Error("Executor's `CommandInfo` is invalid: " + error->message);
    }
  }

  return None();
}


Option<Error> validate(
    const ExecutorInfo& executor,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  Option<Error> error = executor::validate(executor);
  if (error.isSome()) {
    return error;
  }

  const vector<lambda::function<Option<Error>()>> executorValidators = {
    lambda::bind(internal::validateFrameworkID, executor, framework),
    lambda::bind(internal::validateResources, executor),
    lambda::bind(
        internal::validateCompatibleExecutorInfo, executor, framework, slave),
  };

  foreach (const auto& validator, executorValidators) {
    error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}

} // namespace internal {

Option<Error> validate(const ExecutorInfo& executor)
{
  const vector<lambda::function<Option<Error>(const ExecutorInfo&)>>
      executorValidators = {
    internal::validateType,
    internal::validateExecutorID,
    internal::validateShutdownGracePeriod,
    internal::validateCommandInfo,
  };

  foreach (const auto& validator, executorValidators) {
    Option<Error> error = validator(executor);
    if (error.isSome()) {
      return error.get();
    }
  }

  return None();
}

} // namespace executor {


namespace task {

namespace internal {

Option<Error> validateTaskID(const TaskInfo& task)
{
  // Delegate to the common TaskID validation. Here we wrap
  // around it to be consistent with other task validators.
  return common::validation::validateTaskID(task.task_id());
}


// Validates that the TaskID does not collide with the ID of a running
// or unreachable task for this framework.
Option<Error> validateUniqueTaskID(const TaskInfo& task, Framework* framework)
{
  const TaskID& taskId = task.task_id();

  if (framework->tasks.contains(taskId)) {
    return Error("Task has duplicate ID: " + taskId.value());
  }

  // TODO(neilc): `unreachableTasks` is a fixed-size cache and is not
  // preserved across master failover, so we cannot avoid all possible
  // task ID collisions (MESOS-6785).
  if (framework->unreachableTasks.contains(taskId)) {
    return Error("Task reuses the ID of an unreachable task: " +
                 taskId.value());
  }

  return None();
}


// Validates that the slave ID used by a task is correct.
Option<Error> validateSlaveID(const TaskInfo& task, Slave* slave)
{
  if (task.slave_id() != slave->id) {
    return Error(
        "Task uses invalid agent " + task.slave_id().value() +
        " while agent " + slave->id.value() + " is expected");
  }

  return None();
}


Option<Error> validateKillPolicy(const TaskInfo& task)
{
  if (task.has_kill_policy() &&
      task.kill_policy().has_grace_period() &&
      Nanoseconds(task.kill_policy().grace_period().nanoseconds()) <
        Duration::zero()) {
    return Error("Task's 'kill_policy.grace_period' must be non-negative");
  }

  return None();
}


Option<Error> validateCheck(const TaskInfo& task)
{
  if (task.has_check()) {
    Option<Error> error = checks::validation::checkInfo(task.check());
    if (error.isSome()) {
      return Error("Task uses invalid check: " + error->message);
    }
  }

  return None();
}


Option<Error> validateHealthCheck(const TaskInfo& task)
{
  if (task.has_health_check()) {
    Option<Error> error = checks::validation::healthCheck(task.health_check());
    if (error.isSome()) {
      return Error("Task uses invalid health check: " + error->message);
    }
  }

  return None();
}


Option<Error> validateResources(const TaskInfo& task)
{
  if (task.resources().empty()) {
    return Error("Task uses no resources");
  }

  Option<Error> error = resource::validate(task.resources());
  if (error.isSome()) {
    return Error("Task uses invalid resources: " + error->message);
  }

  const Resources& resources = task.resources();

  error = resource::validateUniquePersistenceID(resources);
  if (error.isSome()) {
    return Error("Task uses duplicate persistence ID: " + error->message);
  }

  error = resource::validateAllocatedToSingleRole(resources);
  if (error.isSome()) {
    return Error("Invalid task resources: " + error->message);
  }

  error = resource::validateRevocableAndNonRevocableResources(resources);
  if (error.isSome()) {
    return Error("Task mixes revocable and non-revocable resources: " +
                 error->message);
  }

  return None();
}


Option<Error> validateTaskAndExecutorResources(const TaskInfo& task)
{
  Resources total = task.resources();
  if (task.has_executor()) {
    total += task.executor().resources();
  }

  Option<Error> error = resource::validate(total);
  if (error.isSome()) {
    return Error(
        "Task and its executor use invalid resources: " + error->message);
  }

  error = resource::validateUniquePersistenceID(total);
  if (error.isSome()) {
    return Error("Task and its executor use duplicate persistence ID: " +
                 error->message);
  }

  error = resource::validateRevocableAndNonRevocableResources(total);
  if (error.isSome()) {
    return Error("Task and its executor mix revocable and non-revocable"
                 " resources: " + error->message);
  }

  return None();
}


// Validates the `CommandInfo` contained within a `TaskInfo`.
Option<Error> validateCommandInfo(const TaskInfo& task)
{
  if (task.has_command()) {
    Option<Error> error =
      common::validation::validateCommandInfo(task.command());
    if (error.isSome()) {
      return Error("Task's `CommandInfo` is invalid: " + error->message);
    }
  }

  return None();
}


// Validates task specific fields except its executor (if it exists).
Option<Error> validateTask(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  // NOTE: The order in which the following validate functions are
  // executed does matter!
  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(internal::validateTaskID, task),
    lambda::bind(internal::validateUniqueTaskID, task, framework),
    lambda::bind(internal::validateSlaveID, task, slave),
    lambda::bind(internal::validateKillPolicy, task),
    lambda::bind(internal::validateCheck, task),
    lambda::bind(internal::validateHealthCheck, task),
    lambda::bind(internal::validateResources, task),
    lambda::bind(internal::validateCommandInfo, task)
  };

  foreach (const lambda::function<Option<Error>()>& validator, validators) {
    Option<Error> error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}


// Validates `Task.executor` if it exists.
Option<Error> validateExecutor(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  if (task.has_executor() == task.has_command()) {
    return Error(
        "Task should have at least one (but not both) of CommandInfo or "
        "ExecutorInfo present");
  }

  Resources total = task.resources();

  Option<Error> error = None();

  if (task.has_executor()) {
    const ExecutorInfo& executor = task.executor();

    // Do the general validation first.
    error = executor::internal::validate(executor, framework, slave);
    if (error.isSome()) {
      return error;
    }

    // Now do specific validation when an executor is specified on `Task`.

    // TODO(vinod): Revisit this when we allow schedulers to explicitly
    // specify 'DEFAULT' executor in the `LAUNCH` operation.

    if (executor.has_type() && executor.type() != ExecutorInfo::CUSTOM) {
      return Error("'ExecutorInfo.type' must be 'CUSTOM'");
    }

    // While `ExecutorInfo.command` is optional in the protobuf,
    // semantically it is still required for backwards compatibility.
    if (!executor.has_command()) {
      return Error("'ExecutorInfo.command' must be set");
    }

    // TODO(martin): MESOS-1807. Return Error instead of logging a
    // warning.
    const Resources& executorResources = executor.resources();

    // Ensure there are no shared resources in the executor resources.
    //
    // TODO(anindya_sinha): For simplicity we currently don't
    // allow shared resources in ExecutorInfo. See comments in
    // `HierarchicalAllocatorProcess::updateAllocation()` for more
    // details. Remove this check once we start supporting it.
    if (!executorResources.shared().empty()) {
      return Error(
          "Executor resources " + stringify(executorResources) +
          " should not contain any shared resources");
    }

    Option<double> cpus =  executorResources.cpus();
    if (cpus.isNone() || cpus.get() < MIN_CPUS) {
      LOG(WARNING)
        << "Executor '" << task.executor().executor_id()
        << "' for task '" << task.task_id()
        << "' uses less CPUs ("
        << (cpus.isSome() ? stringify(cpus.get()) : "None")
        << ") than the minimum required (" << MIN_CPUS
        << "). Please update your executor, as this will be mandatory "
        << "in future releases.";
    }

    Option<Bytes> mem = executorResources.mem();
    if (mem.isNone() || mem.get() < MIN_MEM) {
      LOG(WARNING)
        << "Executor '" << task.executor().executor_id()
        << "' for task '" << task.task_id()
        << "' uses less memory ("
        << (mem.isSome() ? stringify(mem.get().megabytes()) : "None")
        << ") than the minimum required (" << MIN_MEM
        << "). Please update your executor, as this will be mandatory "
        << "in future releases.";
    }

    if (!slave->hasExecutor(framework->id(), task.executor().executor_id())) {
      total += executorResources;
    }
  }

  // Now validate combined resources of task and executor.

  // NOTE: This is refactored into a separate function
  // so that it can be easily unit tested.
  error = task::internal::validateTaskAndExecutorResources(task);
  if (error.isSome()) {
    return error;
  }

  if (!offered.contains(total)) {
    return Error(
        "Total resources " + stringify(total) + " required by task and its"
        " executor is more than available " + stringify(offered));
  }

  return None();
}

} // namespace internal {


// Validate task and its executor (if it exists).
Option<Error> validate(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(internal::validateTask, task, framework, slave),
    lambda::bind(internal::validateExecutor, task, framework, slave, offered)
  };

  foreach (const lambda::function<Option<Error>()>& validator, validators) {
    Option<Error> error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}


namespace group {

namespace internal {

Option<Error> validateTask(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  // Do the general validation first.
  Option<Error> error = task::internal::validateTask(task, framework, slave);
  if (error.isSome()) {
    return error;
  }

  // Now do `TaskGroup` specific validation.

  if (!task.has_executor()) {
    return Error("'TaskInfo.executor' must be set");
  }

  if (task.has_container()) {
    if (task.container().network_infos().size() > 0) {
      return Error("NetworkInfos must not be set on the task");
    }

    if (task.container().type() == ContainerInfo::DOCKER) {
      return Error("Docker ContainerInfo is not supported on the task");
    }
  }

  return None();
}


Option<Error> validateTaskGroupAndExecutorResources(
    const TaskGroupInfo& taskGroup,
    const ExecutorInfo& executor)
{
  Resources total = executor.resources();
  foreach (const TaskInfo& task, taskGroup.tasks()) {
    total += task.resources();
  }

  Option<Error> error = resource::validateUniquePersistenceID(total);
  if (error.isSome()) {
    return Error("Task group and executor use duplicate persistence ID: " +
                 error->message);
  }

  error = resource::validateRevocableAndNonRevocableResources(total);
  if (error.isSome()) {
    return Error("Task group and executor mix revocable and non-revocable"
                 " resources: " + error->message);
  }

  return None();
}


Option<Error> validateExecutor(
    const TaskGroupInfo& taskGroup,
    const ExecutorInfo& executor,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  // Do the general validation first.
  Option<Error> error =
    executor::internal::validate(executor, framework, slave);

  if (error.isSome()) {
    return error;
  }

  // Now do `TaskGroup` specific validation.

  if (!executor.has_type()) {
    return Error("'ExecutorInfo.type' must be set");
  }

  if (executor.type() == ExecutorInfo::UNKNOWN) {
    return Error("Unknown executor type");
  }

  if (executor.has_container() &&
      executor.container().type() == ContainerInfo::DOCKER) {
    return Error("Docker ContainerInfo is not supported on the executor");
  }

  // Validate the `ExecutorInfo` in all tasks are same.

  foreach (const TaskInfo& task, taskGroup.tasks()) {
    if (task.has_executor() && task.executor() != executor) {
      return Error(
          "The `ExecutorInfo` of "
          "task '" + stringify(task.task_id()) + "' is different from "
          "executor '" + stringify(executor.executor_id()) + "'");
    }
  }

  const Resources& executorResources = executor.resources();

  // Validate minimal cpus and memory resources of executor.
  Option<double> cpus =  executorResources.cpus();
  if (cpus.isNone() || cpus.get() < MIN_CPUS) {
    return Error(
      "Executor '" + stringify(executor.executor_id()) +
      "' uses less CPUs (" +
      (cpus.isSome() ? stringify(cpus.get()) : "None") +
      ") than the minimum required (" + stringify(MIN_CPUS) + ")");
  }

  Option<Bytes> mem = executorResources.mem();
  if (mem.isNone() || mem.get() < MIN_MEM) {
    return Error(
      "Executor '" + stringify(executor.executor_id()) +
      "' uses less memory (" +
      (mem.isSome() ? stringify(mem.get().megabytes()) : "None") +
      ") than the minimum required (" + stringify(MIN_MEM) + ")");
  }

  Option<Bytes> disk = executorResources.disk();
  if (disk.isNone()) {
    return Error(
      "Executor '" + stringify(executor.executor_id()) + "' uses no disk");
  }

  // Validate combined resources of task group and executor.

  // NOTE: This is refactored into a separate function so that it can
  // be easily unit tested.
  error = internal::validateTaskGroupAndExecutorResources(taskGroup, executor);
  if (error.isSome()) {
    return error;
  }

  Resources total;
  foreach (const TaskInfo& task, taskGroup.tasks()) {
    total += task.resources();
  }

  if (!slave->hasExecutor(framework->id(), executor.executor_id())) {
    total += executorResources;
  }

  if (!offered.contains(total)) {
    return Error(
        "Total resources " + stringify(total) + " required by task group and"
        " its executor are more than available " + stringify(offered));
  }

  if (executor.has_command()) {
    Option<Error> error =
      common::validation::validateCommandInfo(executor.command());
    if (error.isSome()) {
      return Error(
          "Executor '" + stringify(executor.executor_id()) + "'" +
          "contains an invalid command: " + error->message);
    }
  }

  return None();
}

} // namespace internal {


Option<Error> validate(
    const TaskGroupInfo& taskGroup,
    const ExecutorInfo& executor,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  foreach (const TaskInfo& task, taskGroup.tasks()) {
    Option<Error> error = internal::validateTask(task, framework, slave);
    if (error.isSome()) {
      return Error("Task '" + stringify(task.task_id()) + "' is invalid: " +
                   error->message);
    }
  }

  Option<Error> error =
    internal::validateExecutor(taskGroup, executor, framework, slave, offered);

  if (error.isSome()) {
    return error;
  }

  return None();
}

} // namespace group {

} // namespace task {


namespace offer {

Offer* getOffer(Master* master, const OfferID& offerId)
{
  CHECK_NOTNULL(master);
  return master->getOffer(offerId);
}


InverseOffer* getInverseOffer(Master* master, const OfferID& offerId)
{
  CHECK_NOTNULL(master);
  return master->getInverseOffer(offerId);
}


Slave* getSlave(Master* master, const SlaveID& slaveId)
{
  CHECK_NOTNULL(master);
  return master->slaves.registered.get(slaveId);
}


Try<SlaveID> getSlaveId(Master* master, const OfferID& offerId)
{
  // Try as an offer.
  Offer* offer = getOffer(master, offerId);
  if (offer != nullptr) {
    return offer->slave_id();
  }

  InverseOffer* inverseOffer = getInverseOffer(master, offerId);
  if (inverseOffer != nullptr) {
    return inverseOffer->slave_id();
  }

  return Error("Offer " + stringify(offerId) + " is no longer valid");
}


Try<FrameworkID> getFrameworkId(Master* master, const OfferID& offerId)
{
  // Try as an offer.
  Offer* offer = getOffer(master, offerId);
  if (offer != nullptr) {
    return offer->framework_id();
  }

  InverseOffer* inverseOffer = getInverseOffer(master, offerId);
  if (inverseOffer != nullptr) {
    return inverseOffer->framework_id();
  }

  return Error("Offer " + stringify(offerId) + " is no longer valid");
}


Option<Error> validateOfferIds(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master)
{
  foreach (const OfferID& offerId, offerIds) {
    Offer* offer = getOffer(master, offerId);
    if (offer == nullptr) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }
  }

  return None();
}


Option<Error> validateInverseOfferIds(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master)
{
  foreach (const OfferID& offerId, offerIds) {
    InverseOffer* inverseOffer = getInverseOffer(master, offerId);
    if (inverseOffer == nullptr) {
      return Error(
          "Inverse offer " + stringify(offerId) + " is no longer valid");
    }
  }

  return None();
}


// Validates that an offer only appears once in offer list.
Option<Error> validateUniqueOfferID(const RepeatedPtrField<OfferID>& offerIds)
{
  hashset<OfferID> offers;

  foreach (const OfferID& offerId, offerIds) {
    if (offers.contains(offerId)) {
      return Error("Duplicate offer " + stringify(offerId) + " in offer list");
    }

    offers.insert(offerId);
  }

  return None();
}


// Validates that all offers belongs to the expected framework.
Option<Error> validateFramework(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework)
{
  foreach (const OfferID& offerId, offerIds) {
    Try<FrameworkID> offerFrameworkId = getFrameworkId(master, offerId);
    if (offerFrameworkId.isError()) {
      return offerFrameworkId.error();
    }

    if (framework->id() != offerFrameworkId.get()) {
      return Error(
          "Offer " + stringify(offerId) +
          " has invalid framework " + stringify(offerFrameworkId.get()) +
          " while framework " + stringify(framework->id()) + " is expected");
    }
  }

  return None();
}


// Validate that all offers in one operation must be
// allocated to the same role.
Option<Error> validateAllocationRole(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master)
{
  Option<string> role;
  foreach (const OfferID& offerId, offerIds) {
    Offer* offer = getOffer(master, offerId);
    if (offer == nullptr) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    CHECK(offer->has_allocation_info());

    string _role = offer->allocation_info().role();
    if (role.isNone()) {
      role = _role;
      continue;
    }

    if (_role != role.get()) {
      return Error(
          "Aggregated offers must be allocated to the same role. Offer " +
          stringify(offerId) + " uses role '" + _role + " but another"
          " is using role '" + role.get());
    }
  }

  return None();
}


// Validates that all offers belong to the same valid slave.
Option<Error> validateSlave(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master)
{
  Option<SlaveID> slaveId;

  foreach (const OfferID& offerId, offerIds) {
    Try<SlaveID> offerSlaveId = getSlaveId(master, offerId);
    if (offerSlaveId.isError()) {
      return offerSlaveId.error();
    }

    Slave* slave = getSlave(master, offerSlaveId.get());

    // This is not possible because the offer should've been removed.
    CHECK(slave != nullptr)
      << "Offer " << offerId
      << " outlived agent " << offerSlaveId.get();

    // This is not possible because the offer should've been removed.
    CHECK(slave->connected)
      << "Offer " << offerId
      << " outlived disconnected agent " << *slave;

    if (slaveId.isNone()) {
      // Set slave id and use as base case for validation.
      slaveId = slave->id;
    }

    if (slave->id != slaveId.get()) {
      return Error(
          "Aggregated offers must belong to one single agent. Offer " +
          stringify(offerId) + " uses agent " +
          stringify(slave->id) + " and agent " +
          stringify(slaveId.get()));
    }
  }

  return None();
}


Option<Error> validate(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework)
{
  CHECK_NOTNULL(master);
  CHECK_NOTNULL(framework);

  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(validateUniqueOfferID, offerIds),
    lambda::bind(validateOfferIds, offerIds, master),
    lambda::bind(validateFramework, offerIds, master, framework),
    lambda::bind(validateAllocationRole, offerIds, master),
    lambda::bind(validateSlave, offerIds, master)
  };

  foreach (const lambda::function<Option<Error>()>& validator, validators) {
    Option<Error> error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}


Option<Error> validateInverseOffers(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework)
{
  CHECK_NOTNULL(master);
  CHECK_NOTNULL(framework);

  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(validateUniqueOfferID, offerIds),
    lambda::bind(validateInverseOfferIds, offerIds, master),
    lambda::bind(validateFramework, offerIds, master, framework),
    lambda::bind(validateSlave, offerIds, master)
  };

  foreach (const lambda::function<Option<Error>()>& validator, validators) {
    Option<Error> error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}

} // namespace offer {


namespace operation {

Option<Error> validate(
    const Offer::Operation::Reserve& reserve,
    const Option<Principal>& principal,
    const Option<FrameworkInfo>& frameworkInfo)
{
  // NOTE: this ensures the reservation is not being made to the "*" role.
  Option<Error> error = resource::validate(reserve.resources());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  Option<hashset<string>> frameworkRoles;
  if (frameworkInfo.isSome()) {
    frameworkRoles = protobuf::framework::getRoles(frameworkInfo.get());
  }

  foreach (const Resource& resource, reserve.resources()) {
    if (!Resources::isDynamicallyReserved(resource)) {
      return Error(
          "Resource " + stringify(resource) + " is not dynamically reserved");
    }

    if (principal.isSome()) {
      // We assume that `principal->value.isSome()` is true. The master's HTTP
      // handlers enforce this constraint, and V0 authenticators will only
      // return principals of that form.
      CHECK_SOME(principal->value);

      if (!resource.reservation().has_principal()) {
        return Error(
            "A reserve operation was attempted by principal '" +
            stringify(principal.get()) + "', but there is a "
            "reserved resource in the request with no principal set");
      }

      if (principal != resource.reservation().principal()) {
        return Error(
            "A reserve operation was attempted by authenticated principal '" +
            stringify(principal.get()) + "', which does not match a "
            "reserved resource in the request with principal '" +
            resource.reservation().principal() + "'");
      }
    }

    if (frameworkRoles.isSome()) {
      // If information on the framework was passed we are dealing
      // with a request over the framework API. In this case we expect
      // that the reserved resources where allocated to the role, and
      // that the allocation role and reservation role match the role
      // of the framework.
      if (!resource.allocation_info().has_role()) {
        return Error(
            "A reserve operation was attempted on unallocated resource"
            " " + stringify(resource) + ", but frameworks can only"
            " perform reservations on allocated resources");
      }

      if (resource.allocation_info().role() != resource.role()) {
        return Error(
            "A reserve operation was attempted for a resource with role"
            " '" + resource.role() + "', but the resource was allocated"
            " to role '" + resource.allocation_info().role() + "'");
      }

      if (!frameworkRoles->contains(resource.allocation_info().role())) {
        return Error(
            "A reserve operation was attempted for a resource allocated"
            " to role '" + resource.role() + "', but the framework only"
            " has roles '" + stringify(frameworkRoles.get()) + "'");
      }

      if (!frameworkRoles->contains(resource.role())) {
        return Error(
            "A reserve operation was attempted for a resource with role"
            " '" + resource.role() + "', but the framework can only"
            " reserve resources with roles"
            " '" + stringify(frameworkRoles.get()) + "'");
      }
    } else {
      // If no `FrameworkInfo` was passed we are dealing with a
      // request via the operator HTTP API. In this case we expect
      // that the reserved resources have no `AllocationInfo` set
      // because operators can only reserve from the unallocated
      // resources.
      if (resource.has_allocation_info()) {
        return Error(
            "A reserve operation was attempted with an allocated resource,"
            " but the operator API only allows reservations to be made to"
            " unallocated resources");
      }
    }

    // NOTE: This check would be covered by 'contains' since there
    // shouldn't be any unreserved resources with 'disk' set.
    // However, we keep this check since it will be a more useful
    // error message than what contains would produce.
    if (Resources::isPersistentVolume(resource)) {
      return Error("A persistent volume " + stringify(resource) +
                   " must already be reserved");
    }
  }

  if (frameworkInfo.isSome()) {
    // If the operation is being applied by a framework, we also
    // ensure that across all the resources, they are allocated
    // to a single role.
    error = resource::validateAllocatedToSingleRole(reserve.resources());
    if (error.isSome()) {
      return Error("Invalid reservation resources: " + error->message);
    }
  }

  return None();
}


Option<Error> validate(const Offer::Operation::Unreserve& unreserve)
{
  Option<Error> error = resource::validate(unreserve.resources());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  // NOTE: We don't check that 'FrameworkInfo.principal' matches
  // 'Resource.ReservationInfo.principal' here because the authorization
  // depends on the "unreserve" ACL which specifies which 'principal' can
  // unreserve which 'principal's resources. In the absence of an ACL, we allow
  // any 'principal' to unreserve any other 'principal's resources.

  foreach (const Resource& resource, unreserve.resources()) {
    if (!Resources::isDynamicallyReserved(resource)) {
      return Error(
          "Resource " + stringify(resource) + " is not dynamically reserved");
    }

    if (Resources::isPersistentVolume(resource)) {
      return Error(
          "A dynamically reserved persistent volume " +
          stringify(resource) +
          " cannot be unreserved directly. Please destroy the persistent"
          " volume first then unreserve the resource");
    }
  }

  return None();
}


Option<Error> validate(
    const Offer::Operation::Create& create,
    const Resources& checkpointedResources,
    const Option<Principal>& principal,
    const Option<FrameworkInfo>& frameworkInfo)
{
  Option<Error> error = resource::validate(create.volumes());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = resource::validatePersistentVolume(create.volumes());
  if (error.isSome()) {
    return Error("Not a persistent volume: " + error.get().message);
  }

  error = resource::validateUniquePersistenceID(
      checkpointedResources + create.volumes());

  if (error.isSome()) {
    return error;
  }

  foreach (const Resource& volume, create.volumes()) {
    // If the volume being created is a shared persistent volume, we
    // allow it only if the framework has opted in for SHARED_RESOURCES.
    if (frameworkInfo.isSome() &&
        volume.has_shared() &&
        !protobuf::frameworkHasCapability(
            frameworkInfo.get(),
            FrameworkInfo::Capability::SHARED_RESOURCES)) {
      return Error(
          "Create volume operation for '" + stringify(volume) +
          "' has been attempted by framework '" +
          stringify(frameworkInfo.get().id()) +
          "' with no SHARED_RESOURCES capability");
    }

    // Ensure that the provided principals match. If `principal` is `None`,
    // we allow `volume.disk.persistence.principal` to take any value.
    if (principal.isSome()) {
      // We assume that `principal->value.isSome()` is true. The master's HTTP
      // handlers enforce this constraint, and V0 authenticators will only
      // return principals of that form.
      CHECK_SOME(principal->value);

      if (!volume.disk().persistence().has_principal()) {
        return Error(
            "Create volume operation attempted by principal '" +
            stringify(principal.get()) + "', but there is a volume in the "
            "operation with no principal set in 'disk.persistence'");
      }

      if (principal != volume.disk().persistence().principal()) {
        return Error(
            "Create volume operation attempted by authenticated principal '" +
            stringify(principal.get()) + "', which does not match "
            "a volume in the operation with principal '" +
            volume.disk().persistence().principal() +
            "' set in 'disk.persistence'");
      }
    }
  }

  if (frameworkInfo.isSome()) {
    // If the operation is being applied by a framework, we also
    // ensure that across all the resources, they are allocated
    // to a single role.
    error = resource::validateAllocatedToSingleRole(create.volumes());
    if (error.isSome()) {
      return Error("Invalid volume resources: " + error->message);
    }
  }

  return None();
}


Option<Error> validate(
    const Offer::Operation::Destroy& destroy,
    const Resources& checkpointedResources,
    const hashmap<FrameworkID, Resources>& usedResources,
    const hashmap<FrameworkID, hashmap<TaskID, TaskInfo>>& pendingTasks)
{
  // The operation can either contain allocated resources
  // (in the case of a framework accepting offers), or
  // unallocated resources (in the case of the operator
  // endpoints). To ensure we can check for the presence
  // of the volume in the resources in use by tasks and
  // executors, we unallocate both the volume and the
  // used resources before performing the contains check.
  //
  // TODO(bmahler): This lambda is copied in several places
  // in the code, consider how best to pull this out.
  auto unallocated = [](const Resources& resources) {
    Resources result = resources;
    result.unallocate();
    return result;
  };

  Resources volumes = unallocated(destroy.volumes());

  Option<Error> error = resource::validate(volumes);
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = resource::validatePersistentVolume(volumes);
  if (error.isSome()) {
    return Error("Not a persistent volume: " + error.get().message);
  }

  if (!checkpointedResources.contains(volumes)) {
    return Error("Persistent volumes not found");
  }

  // Ensure the volumes being destroyed are not in use currently.
  // This check is mainly to validate destruction of shared volumes since
  // it is not possible for a non-shared resource to appear in an offer
  // if it is already in use.
  foreachvalue (const Resources& resources, usedResources) {
    foreach (const Resource& volume, volumes) {
      if (unallocated(resources).contains(volume)) {
        return Error("Persistent volumes in use");
      }
    }
  }

  // Ensure that the volumes being destroyed are not requested by any pending
  // task. This check is mainly to validate destruction of shared volumes.
  // Note that resource requirements in pending tasks are not validated yet
  // so it is possible that the DESTROY validation fails due to invalid
  // pending tasks.
  typedef hashmap<TaskID, TaskInfo> TaskMap;
  foreachvalue(const TaskMap& tasks, pendingTasks) {
    foreachvalue (const TaskInfo& task, tasks) {
      Resources resources = task.resources();
      if (task.has_executor()) {
        resources += task.executor().resources();
      }

      foreach (const Resource& volume, destroy.volumes()) {
        if (unallocated(resources).contains(volume)) {
          return Error("Persistent volume in pending tasks");
        }
      }
    }
  }

  return None();
}

} // namespace operation {

} // namespace validation {
} // namespace master {
} // namespace internal {
} // namespace mesos {
