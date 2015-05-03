/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
static bool invalid(char c)
{
  return iscntrl(c) || c == '/' || c == '\\';
}

namespace resource {

// Validates that all the given resources are dynamically-reserved.
Option<Error> validateDynamicReservation(
    const RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    if (!resource.has_reservation()) {
      return Error(
          "Resource " + stringify(resource) +
          " does not have the 'reservation' field set");
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
      if (std::count_if(id.begin(), id.end(), invalid) > 0) {
        return Error("Persistence ID '" + id + "' contains invalid characters");
      }
    } else if (resource.disk().has_volume()) {
      return Error("Non-persistent volume not supported");
    } else {
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

  error = validateDiskInfo(resources);
  if (error.isSome()) {
    return Error("Invalid DiskInfo: " + error.get().message);
  }

  return None();
}

} // namespace resource {


namespace task {

// Validates that a task id is valid, i.e., contains only valid
// characters.
Option<Error> validateTaskID(const TaskInfo& task)
{
  const string& id = task.task_id().value();

  if (std::count_if(id.begin(), id.end(), invalid) > 0) {
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
        "Task uses invalid slave " + task.slave_id().value() +
        " while slave " + slave->id.value() + " is expected");
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
    // TODO(bmahler): Set this field in the master instead of
    // depending on the scheduler driver do it.
    if (!task.executor().has_framework_id()) {
      return Error(
          "Task has invalid ExecutorInfo: missing field 'framework_id'");
    }

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
  }

  return None();
}


// Validates that a task that asks for checkpointing is not being
// launched on a slave that has not enabled checkpointing.
// TODO(jieyu): Remove this in favor of a CHECK, because the allocator
// should filter these out.
Option<Error> validateCheckpoint(Framework* framework, Slave* slave)
{
  if (framework->info.checkpoint() && !slave->info.checkpoint()) {
    return Error(
        "Task asked to be checkpointed but slave " +
        stringify(slave->id) + " has checkpointing disabled");
  }

  return None();
}

// Validates that the resources specified by the framework are valid.
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

  error = resource::validateUniquePersistenceID(total);
  if (error.isSome()) {
    return error;
  }

  return None();
}


// Validates that the task and the executor are using proper amount of
// resources. For instance, the used resources by a task on each slave
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
  vector<lambda::function<Option<Error>(void)>> validators = {
    lambda::bind(validateTaskID, task),
    lambda::bind(validateUniqueTaskID, task, framework),
    lambda::bind(validateSlaveID, task, slave),
    lambda::bind(validateExecutorInfo, task, framework, slave),
    lambda::bind(validateCheckpoint, framework, slave),
    lambda::bind(validateResources, task),
    lambda::bind(validateResourceUsage, task, framework, slave, offered)
  };

  // TODO(benh): Add a validateHealthCheck function.

  // TODO(jieyu): Add a validateCommandInfo function.

  foreach (const lambda::function<Option<Error>(void)>& validator, validators) {
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


Slave* getSlave(Master* master, const SlaveID& slaveId)
{
  CHECK_NOTNULL(master);
  return master->getSlave(slaveId);
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
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    if (framework->id() != offer->framework_id()) {
      return Error(
          "Offer " + stringify(offer->id()) +
          " has invalid framework " + stringify(offer->framework_id()) +
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
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    Slave* slave = getSlave(master, offer->slave_id());

    // This is not possible because the offer should've been removed.
    CHECK(slave != NULL)
      << "Offer " << offerId
      << " outlived slave " << offer->slave_id();

    // This is not possible because the offer should've been removed.
    CHECK(slave->connected)
      << "Offer " << offerId
      << " outlived disconnected slave " << *slave;

    if (slaveId.isNone()) {
      // Set slave id and use as base case for validation.
      slaveId = slave->id;
    }

    if (slave->id != slaveId.get()) {
      return Error(
          "Aggregated offers must belong to one single slave. Offer " +
          stringify(offerId) + " uses slave " +
          stringify(slave->id) + " and slave " +
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

  vector<lambda::function<Option<Error>(void)>> validators = {
    lambda::bind(validateUniqueOfferID, offerIds),
    lambda::bind(validateFramework, offerIds, master, framework),
    lambda::bind(validateSlave, offerIds, master)
  };

  foreach (const lambda::function<Option<Error>(void)>& validator, validators) {
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
    const string& role,
    const Option<string>& principal)
{
  Option<Error> error = resource::validate(reserve.resources());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = resource::validateDynamicReservation(reserve.resources());
  if (error.isSome()) {
    return Error("Not a dynamic reservation: " + error.get().message);
  }

  if (principal.isNone()) {
    return Error("A framework without a principal cannot reserve resources.");
  }

  foreach (const Resource& resource, reserve.resources()) {
    if (resource.role() != role) {
      return Error(
          "The reserved resource's role '" + resource.role() +
          "' does not match the framework's role '" + role + "'");
    }

    if (resource.reservation().principal() != principal.get()) {
      return Error(
          "The reserved resource's principal '" +
          stringify(resource.reservation().principal()) +
          "' does not match the framework's principal '" +
          stringify(principal.get()) + "'");
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


Option<Error> validate(
    const Offer::Operation::Unreserve& unreserve,
    bool hasPrincipal)
{
  Option<Error> error = resource::validate(unreserve.resources());
  if (error.isSome()) {
    return Error("Invalid resources: " + error.get().message);
  }

  error = resource::validateDynamicReservation(unreserve.resources());
  if (error.isSome()) {
    return Error("Not a dynamic reservation: " + error.get().message);
  }

  if (!hasPrincipal) {
    return Error("A framework without a principal cannot unreserve resources.");
  }

  // NOTE: We don't check that 'FrameworkInfo.principal' matches
  // 'Resource.ReservationInfo.principal' here because the authorization
  // depends on the "unreserve" ACL which specifies which 'principal' can
  // unreserve which 'principal's resources. In the absense of an ACL, we allow
  // any 'principal' to unreserve any other 'principal's resources.

  foreach (const Resource& resource, unreserve.resources()) {
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
    const Resources& checkpointedResources)
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
