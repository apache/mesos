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

#include <algorithm>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/type_utils.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/stringify.hpp>

#include "master/master.hpp"
#include "master/validation.hpp"

using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace internal {
namespace master {
namespace validation {

// A helper function which returns true if the given character is not
// suitable for an ID.
static bool invalidCharacter(char c)
{
  return iscntrl(c) || c == '/' || c == '\\';
}


namespace master {
namespace call {

Option<Error> validate(
    const mesos::master::Call& call,
    const Option<string>& principal)
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

    case mesos::master::Call::GET_STATE_SUMMARY:
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

    case mesos::master::Call::GET_LEADING_MASTER:
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

    case mesos::master::Call::SUBSCRIBE:
      return None();
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace master {

namespace scheduler {
namespace call {

Option<Error> validate(
    const mesos::scheduler::Call& call,
    const Option<string>& principal)
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
      return Error(
          "Authenticated principal '" + principal.get() + "' does not "
          "match principal '" + frameworkInfo.principal() + "' set in "
          "`FrameworkInfo`");
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
      if (resource.disk().volume().mode() == Volume::RO) {
        return Error("Read-only persistent volume not supported");
      }
      if (resource.disk().volume().has_host_path()) {
        return Error("Expecting 'host_path' to be unset for persistent volume");
      }

      // Ensure persistence ID does not have invalid characters.
      string id = resource.disk().persistence().id();
      if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
        return Error("Persistence ID '" + id + "' contains invalid characters");
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
    const RepeatedPtrField<Resource>& resources)
{
  hashmap<string, hashset<string>> persistenceIds;

  // Check duplicated persistence ID within the given resources.
  Resources volumes = Resources(resources).persistentVolumes();

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


// Validates that all the given resources are persistent volumes.
Option<Error> validatePersistentVolume(
    const RepeatedPtrField<Resource>& volumes)
{
  foreach (const Resource& volume, volumes) {
    if (!volume.has_disk()) {
      return Error("Resource " + stringify(volume) + " does not have DiskInfo");
    } else if (!volume.disk().has_persistence()) {
      return Error("'persistence' is not set in DiskInfo");
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


namespace task {

namespace internal {

// Validates that a task id is valid, i.e., contains only valid
// characters.
Option<Error> validateTaskID(const TaskInfo& task)
{
  const string& id = task.task_id().value();

  if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
    return Error("TaskID '" + id + "' contains invalid characters");
  }

  return None();
}


// Validates that the TaskID does not collide with any existing tasks
// for the framework.
Option<Error> validateUniqueTaskID(const TaskInfo& task, Framework*
    framework)
{
  const TaskID& taskId = task.task_id();

  if (framework->tasks.contains(taskId)) {
    return Error("Task has duplicate ID: " + taskId.value());
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


// Validates that tasks that use the "same" executor (i.e., same
// ExecutorID) have an identical ExecutorInfo.
Option<Error> validateExecutorInfo(
    const TaskInfo& task, Framework* framework, Slave* slave)
{
  if (task.has_executor() == task.has_command()) {
    return Error(
        "Task should have at least one (but not both) of CommandInfo or "
        "ExecutorInfo present");
  }

  if (task.has_executor()) {
    // The master currently expects ExecutorInfo.framework_id to be
    // set even though it is an optional field. Currently, the
    // scheduler driver ensures that the field is set. For schedulers
    // not using the driver, we need to do the validation here.
    CHECK(task.executor().has_framework_id());

    if (task.executor().framework_id() != framework->id()) {
      return Error(
          "ExecutorInfo has an invalid FrameworkID"
          " (Actual: " + stringify(task.executor().framework_id()) +
          " vs Expected: " + stringify(framework->id()) + ")");
    }

    const ExecutorID& executorId = task.executor().executor_id();
    Option<ExecutorInfo> executorInfo = None();

    if (slave->hasExecutor(framework->id(), executorId)) {
      executorInfo =
        slave->executors.get(framework->id()).get().get(executorId);
    }

    if (executorInfo.isSome() && !(task.executor() == executorInfo.get())) {
      return Error(
          "Task has invalid ExecutorInfo (existing ExecutorInfo"
          " with same ExecutorID is not compatible).\n"
          "------------------------------------------------------------\n"
          "Existing ExecutorInfo:\n" +
          stringify(executorInfo.get()) + "\n"
          "------------------------------------------------------------\n"
          "Task's ExecutorInfo:\n" +
          stringify(task.executor()) + "\n"
          "------------------------------------------------------------\n");
    }

    // Make sure provided duration is non-negative.
    if (task.executor().has_shutdown_grace_period() &&
        Nanoseconds(task.executor().shutdown_grace_period().nanoseconds()) <
          Duration::zero()) {
      return Error(
          "ExecutorInfo's 'shutdown_grace_period' must be non-negative");
    }
  }

  return None();
}


// Validates that the task and the executor are using proper amount of
// resources. For instance, the used resources by a task on a slave
// should not exceed the total resources offered on that slave.
Option<Error> validateResourceUsage(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  Resources taskResources = task.resources();

  if (taskResources.empty()) {
    return Error("Task uses no resources");
  }

  Resources executorResources;
  if (task.has_executor()) {
    executorResources = task.executor().resources();
  }

  // Validate minimal cpus and memory resources of executor and log
  // warnings if not set.
  if (task.has_executor()) {
    // TODO(martin): MESOS-1807. Return Error instead of logging a
    // warning in 0.22.0.
    Option<double> cpus =  executorResources.cpus();
    if (cpus.isNone() || cpus.get() < MIN_CPUS) {
      LOG(WARNING)
        << "Executor " << stringify(task.executor().executor_id())
        << " for task " << stringify(task.task_id())
        << " uses less CPUs ("
        << (cpus.isSome() ? stringify(cpus.get()) : "None")
        << ") than the minimum required (" << MIN_CPUS
        << "). Please update your executor, as this will be mandatory "
        << "in future releases.";
    }

    Option<Bytes> mem = executorResources.mem();
    if (mem.isNone() || mem.get() < MIN_MEM) {
      LOG(WARNING)
        << "Executor " << stringify(task.executor().executor_id())
        << " for task " << stringify(task.task_id())
        << " uses less memory ("
        << (mem.isSome() ? stringify(mem.get().megabytes()) : "None")
        << ") than the minimum required (" << MIN_MEM
        << "). Please update your executor, as this will be mandatory "
        << "in future releases.";
    }
  }

  // Validate if resources needed by the task (and its executor in
  // case the executor is new) are available.
  Resources total = taskResources;
  if (!slave->hasExecutor(framework->id(), task.executor().executor_id())) {
    total += executorResources;
  }

  if (!offered.contains(total)) {
    return Error(
        "Task uses more resources " + stringify(total) +
        " than available " + stringify(offered));
  }

  return None();
}


// Validates that the resources specified by the task are valid.
Option<Error> validateResources(const TaskInfo& task)
{
  Option<Error> error = resource::validate(task.resources());
  if (error.isSome()) {
    return Error("Task uses invalid resources: " + error.get().message);
  }

  Resources total = task.resources();

  if (task.has_executor()) {
    error = resource::validate(task.executor().resources());
    if (error.isSome()) {
      return Error("Executor uses invalid resources: " + error.get().message);
    }

    total += task.executor().resources();
  }

  // A task and its executor can either use non-revocable resources
  // or revocable resources of a given name but not both.
  foreach (const string& name, total.names()) {
    Resources resources = total.get(name);
    if (!resources.revocable().empty() &&
        resources != resources.revocable()) {
      return Error("Task (and its executor, if exists) uses both revocable and"
                   " non-revocable " + name);
    }
  }

  error = resource::validateUniquePersistenceID(total);
  if (error.isSome()) {
    return error;
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


} // namespace internal {


Option<Error> validate(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  // NOTE: The order in which the following validate functions are
  // executed does matter! For example, 'validateResourceUsage'
  // assumes that ExecutorInfo is valid which is verified by
  // 'validateExecutorInfo'.
  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(internal::validateTaskID, task),
    lambda::bind(internal::validateUniqueTaskID, task, framework),
    lambda::bind(internal::validateSlaveID, task, slave),
    lambda::bind(internal::validateExecutorInfo, task, framework, slave),
    lambda::bind(internal::validateResources, task),
    lambda::bind(internal::validateKillPolicy, task),
    lambda::bind(
        internal::validateResourceUsage, task, framework, slave, offered)
  };

  // TODO(benh): Add a validateHealthCheck function.

  // TODO(jieyu): Add a validateCommandInfo function.

  foreach (const lambda::function<Option<Error>()>& validator, validators) {
    Option<Error> error = validator();
    if (error.isSome()) {
      return error;
    }
  }

  return None();
}

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

  return Error("Offer id no longer valid");
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

  return Error("Offer id no longer valid");
}


Option<Error> validateOfferIds(
    Master* master,
    const RepeatedPtrField<OfferID>& offerIds)
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
    Master* master,
    const RepeatedPtrField<OfferID>& offerIds)
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


// Validates that all offers belong to the same valid slave.
Option<Error> validateSlave(
    const RepeatedPtrField<OfferID>& offerIds, Master* master)
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
    lambda::bind(validateOfferIds, master, offerIds),
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


Option<Error> validateInverseOffers(
    const RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework)
{
  CHECK_NOTNULL(master);
  CHECK_NOTNULL(framework);

  vector<lambda::function<Option<Error>()>> validators = {
    lambda::bind(validateUniqueOfferID, offerIds),
    lambda::bind(validateInverseOfferIds, master, offerIds),
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
    const Option<string>& principal)
{
  Option<Error> error = resource::validate(reserve.resources());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  foreach (const Resource& resource, reserve.resources()) {
    if (!Resources::isDynamicallyReserved(resource)) {
      return Error(
          "Resource " + stringify(resource) + " is not dynamically reserved");
    }

    if (principal.isSome()) {
      if (!resource.reservation().has_principal()) {
        return Error(
            "A reserve operation was attempted by principal '" +
            principal.get() + "', but there is a reserved resource in the "
            "request with no principal set in `ReservationInfo`");
      }

      if (resource.reservation().principal() != principal.get()) {
        return Error(
            "A reserve operation was attempted by principal '" +
            principal.get() + "', but there is a reserved resource in the "
            "request with principal '" + resource.reservation().principal() +
            "' set in `ReservationInfo`");
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
  // unreserve which 'principal's resources. In the absense of an ACL, we allow
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
    const Option<string>& principal)
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

  // Ensure that the provided principals match. If `principal` is `None`, then
  // we allow `volume.disk().persistence().principal()` to take any value.
  foreach (const Resource& volume, create.volumes()) {
    if (principal.isSome()) {
      if (!volume.disk().persistence().has_principal()) {
        return Error(
            "Create volume operation has been attempted by principal '" +
            principal.get() + "', but there is a volume in the operation with "
            "no principal set in 'DiskInfo.Persistence'");
      }

      if (volume.disk().persistence().principal() != principal.get()) {
        return Error(
            "Create volume operation has been attempted by principal '" +
            principal.get() + "', but there is a volume in the operation with "
            "principal '" + volume.disk().persistence().principal() +
            "' set in 'DiskInfo.Persistence'");
      }
    }
  }

  return None();
}


Option<Error> validate(
    const Offer::Operation::Destroy& destroy,
    const Resources& checkpointedResources)
{
  Option<Error> error = resource::validate(destroy.volumes());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = resource::validatePersistentVolume(destroy.volumes());
  if (error.isSome()) {
    return Error("Not a persistent volume: " + error.get().message);
  }

  if (!checkpointedResources.contains(destroy.volumes())) {
    return Error("Persistent volumes not found");
  }

  return None();
}

} // namespace operation {

} // namespace validation {
} // namespace master {
} // namespace internal {
} // namespace mesos {
