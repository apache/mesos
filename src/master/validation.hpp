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

#ifndef __MASTER_VALIDATION_HPP__
#define __MASTER_VALIDATION_HPP__

#include <google/protobuf/repeated_field.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/master/master.hpp>

#include <process/authenticator.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace master {

class Master;

struct Framework;
struct Slave;

namespace validation {

namespace master {
namespace call {

// Validates that a master:Call is well-formed.
// TODO(bmahler): Add unit tests.
Option<Error> validate(
    const mesos::master::Call& call,
    const Option<process::http::authentication::Principal>& principal = None());

} // namespace call {
} // namespace master {


namespace framework {
namespace internal {

// Validates the roles in given FrameworkInfo. Role, roles and
// MULTI_ROLE should be set according to following matrix. Also,
// roles should not contain duplicate entries.
//
// -- MULTI_ROLE is NOT set --
// +-------+-------+---------+
// |       |Roles  |No Roles |
// +-------+-------+---------+
// |Role   | Error |  None   |
// +-------+-------+---------+
// |No Role| Error |  None   |
// +-------+-------+---------+
//
// ---- MULTI_ROLE is set ----
// +-------+-------+---------+
// |       |Roles  |No Roles |
// +-------+-------+---------+
// |Role   | Error |  Error  |
// +-------+-------+---------+
// |No Role| None  |  None   |
// +-------+-------+---------+
Option<Error> validateRoles(const mesos::FrameworkInfo& frameworkInfo);

} // namespace internal {

// Validate a FrameworkInfo.
//
// TODO(jay_guo): This currently only validates
// the role(s), validate more fields!
Option<Error> validate(const mesos::FrameworkInfo& frameworkInfo);

} // namespace framework {


namespace scheduler {
namespace call {

// Validates that a scheduler::Call is well-formed.
// TODO(bmahler): Add unit tests.
Option<Error> validate(
    const mesos::scheduler::Call& call,
    const Option<process::http::authentication::Principal>& principal = None());

} // namespace call {
} // namespace scheduler {


namespace resource {

// Validates resources specified by frameworks.
// NOTE: We cannot take 'Resources' here because invalid resources are
// silently ignored within its constructor.
Option<Error> validate(
    const google::protobuf::RepeatedPtrField<Resource>& resources);

} // namespace resource {


namespace executor {

// Functions in this namespace are only exposed for testing.
namespace internal {

Option<Error> validateExecutorID(const ExecutorInfo& executor);

// Validates that fields are properly set depending on the type of the executor.
Option<Error> validateType(const ExecutorInfo& executor);

// Validates resources of the executor.
Option<Error> validateResources(const ExecutorInfo& executor);

} // namespace internal {
} // namespace executor {


namespace task {

// Validates a task that a framework attempts to launch within the
// offered resources. Returns an optional error which will cause the
// master to send a `TASK_ERROR` status update back to the framework.
//
// NOTE: This function must be called sequentially for each task, and
// each task needs to be launched before the next can be validated.
Option<Error> validate(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered);


// Functions in this namespace are only exposed for testing.
namespace internal {

// Validates resources of the task.
Option<Error> validateResources(const TaskInfo& task);

// Validates resources of the task and its executor.
Option<Error> validateTaskAndExecutorResources(const TaskInfo& task);

// Validates the kill policy of the task.
Option<Error> validateKillPolicy(const TaskInfo& task);

// Validates the check of the task.
Option<Error> validateCheck(const TaskInfo& task);

// Validates the health check of the task.
Option<Error> validateHealthCheck(const TaskInfo& task);

} // namespace internal {

namespace group {

// Validates a task group that a framework attempts to launch within the
// offered resources. Returns an optional error which will cause the
// master to send a `TASK_ERROR` status updates for *all* the tasks in
// the task group back to the framework.
//
// NOTE: Validation error of *any* task will cause all the tasks in the task
// group to be rejected by the master.
Option<Error> validate(
    const TaskGroupInfo& taskGroup,
    const ExecutorInfo& executor,
    Framework* framework,
    Slave* slave,
    const Resources& offered);


// Functions in this namespace are only exposed for testing.
namespace internal {

// Validates that the resources specified by
// the task group and its executor are valid.
//
// TODO(vinod): Consolidate this with `validateTaskAndExecutorResources()`.
Option<Error> validateTaskGroupAndExecutorResources(
    const TaskGroupInfo& taskGroup,
    const ExecutorInfo& executor);

} // namespace internal {

} // namespace group {

} // namespace task {


namespace offer {

// NOTE: These two functions are placed in the header file because we
// need to declare them as friends of Master.
Offer* getOffer(Master* master, const OfferID& offerId);
InverseOffer* getInverseOffer(Master* master, const OfferID& offerId);
Slave* getSlave(Master* master, const SlaveID& slaveId);


// Validates the given offers.
Option<Error> validate(
    const google::protobuf::RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework);


// Validates the given inverse offers.
Option<Error> validateInverseOffers(
    const google::protobuf::RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework);

} // namespace offer {


namespace operation {

// Validates the RESERVE operation.
Option<Error> validate(
    const Offer::Operation::Reserve& reserve,
    const Option<process::http::authentication::Principal>& principal,
    const Option<FrameworkInfo>& frameworkInfo = None());


// Validates the UNRESERVE operation.
Option<Error> validate(const Offer::Operation::Unreserve& unreserve);


// Validates the CREATE operation. We need slave's checkpointed resources so
// that we can validate persistence ID uniqueness, and we need the principal to
// verify that it's equal to the one in `DiskInfo.Persistence.principal`.
// We need the FrameworkInfo (unless the operation is requested by the
// operator) to ensure shared volumes are created by frameworks with the
// appropriate capability.
Option<Error> validate(
    const Offer::Operation::Create& create,
    const Resources& checkpointedResources,
    const Option<process::http::authentication::Principal>& principal,
    const Option<FrameworkInfo>& frameworkInfo = None());


// Validates the DESTROY operation. We need slave's checkpointed
// resources to validate that the volumes to destroy actually exist.
// We also check that the volumes are not being used, or not assigned
// to any pending task.
Option<Error> validate(
    const Offer::Operation::Destroy& destroy,
    const Resources& checkpointedResources,
    const hashmap<FrameworkID, Resources>& usedResources,
    const hashmap<FrameworkID, hashmap<TaskID, TaskInfo>>& pendingTasks);

} // namespace operation {

} // namespace validation {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_VALIDATION_HPP__
