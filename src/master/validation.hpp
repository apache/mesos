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

#include <stout/error.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace master {

class Master;

struct Framework;
struct Slave;

namespace validation {

namespace scheduler {
namespace call {

// Validates that a scheduler call is well-formed.
// TODO(bmahler): Add unit tests.
Option<Error> validate(const mesos::scheduler::Call& call);

} // namespace call {
} // namespace scheduler {

namespace resource {

// Validates resources specified by frameworks.
// NOTE: We cannot take 'Resources' here because invalid resources are
// silently ignored within its constructor.
Option<Error> validate(
    const google::protobuf::RepeatedPtrField<Resource>& resources);

} // namespace resource {


namespace task {

// Validates a task that a framework attempts to launch within the
// offered resources. Returns an optional error which will cause the
// master to send a failed status update back to the framework.
// NOTE: This function must be called sequentially for each task, and
// each task needs to be launched before the next can be validated.
Option<Error> validate(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offered);


// Functions in this namespace are only exposed for testing.
namespace internal {

// Validates resources of the task and executor (if present).
Option<Error> validateResources(const TaskInfo& task);

// Validates the kill policy of the task.
Option<Error> validateKillPolicy(const TaskInfo& task);

} // namespace internal {

} // namespace task {


namespace offer {

// NOTE: These two functions are placed in the header file because we
// need to declare them as friends of Master.
Offer* getOffer(Master* master, const OfferID& offerId);
Slave* getSlave(Master* master, const SlaveID& slaveId);


// Validates the given offers.
Option<Error> validate(
    const google::protobuf::RepeatedPtrField<OfferID>& offerIds,
    Master* master,
    Framework* framework);

} // namespace offer {


namespace operation {

// Validates the RESERVE operation.
Option<Error> validate(
    const Offer::Operation::Reserve& reserve,
    const Option<std::string>& principal);


// Validates the UNRESERVE operation.
Option<Error> validate(const Offer::Operation::Unreserve& unreserve);


// Validates the CREATE operation. We need slave's checkpointed
// resources so that we can validate persistence ID uniqueness.
Option<Error> validate(
    const Offer::Operation::Create& create,
    const Resources& checkpointedResources);


// Validates the DESTROY operation. We need slave's checkpointed
// resources to validate that the volumes to destroy actually exist.
Option<Error> validate(
    const Offer::Operation::Destroy& destroy,
    const Resources& checkpointedResources);

} // namespace operation {

} // namespace validation {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_VALIDATION_HPP__
