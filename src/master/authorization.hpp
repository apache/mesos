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

#ifndef __MASTER_AUTHORIZATION_HPP__
#define __MASTER_AUTHORIZATION_HPP__

#include <ostream>
#include <string>
#include <vector>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/authorizer/authorizer.pb.h>

namespace mesos {
namespace authorization {


class ActionObject
{
public:
  Action action() const { return action_; }

  const Option<Object>& object() const& { return object_; }
  Option<Object>&& object() && { return std::move(object_); }

  // Returns action-object pair for authorizing a task launch.
  static ActionObject taskLaunch(
      const TaskInfo& task,
      const FrameworkInfo& framework);

  // Returns action-object pair for authorizing
  // framework (re)registration or update.
  static ActionObject frameworkRegistration(
      const FrameworkInfo& frameworkInfo);

  // Returns action-object pair(s) for authorizing agent re(registration).
  static std::vector<ActionObject> agentRegistration(
      const SlaveInfo& slaveInfo);

  // Methods that return action-object pair(s) for authorizing
  // offer operations other than LAUNCH/LAUNCH_GROUP.
  //
  // NOTE: these methods rely on the caller to ensure that operation
  // resources are in post-reservation-refinement format.
  // Otherwise, they  may crash the program.

  static std::vector<ActionObject> unreserve(
      const Offer::Operation::Unreserve& unreserve);

  static std::vector<ActionObject> reserve(
      const Offer::Operation::Reserve& reserve);

  static std::vector<ActionObject> createVolume(
      const Offer::Operation::Create& create);

  static std::vector<ActionObject> destroyVolume(
      const Offer::Operation::Destroy& destroy);

  static ActionObject growVolume(
      const Offer::Operation::GrowVolume& grow);

  static ActionObject shrinkVolume(
      const Offer::Operation::ShrinkVolume& shrink);

  // Returns Error if disk type is not supported.
  //
  // TODO(asekretenko): Change return type to ActionObject after
  // authorization of invalid operations no longer occurs (see MESOS-10083).
  static Try<ActionObject> createDisk(
      const Offer::Operation::CreateDisk& createDisk);

  // Returns Error if disk type is not supported.
  //
  // TODO(asekretenko): Change return type to ActionObject after
  // authorization of invalid operations no longer occurs (see MESOS-10083).
  static Try<ActionObject> destroyDisk(
      const Offer::Operation::DestroyDisk& destroyDisk);

private:
  Action action_;
  Option<Object> object_;

  ActionObject(Action action, Option<Object>&& object)
    : action_(action), object_(object){};

  // Returns an action-object pair for the case that commonly
  // occurs when authorizing non-LAUNCH operations: sets `action`,
  // `object.resource` and `object.value`.
  //
  // NOTE: the deprecated `object.value` field is set to support legacy
  // authorizers that have not been upgraded to look at `object.resource`.
  static ActionObject fromResourceWithLegacyValue(
      Action action,
      const Resource& resource,
      std::string value_);

  // Helper method for `reserve()`/`unreserve()`.
  //
  // For each resource reserved with a principal, pushes into `result` an
  // ActionObject needed for unreserving it. If there are resources reserved
  // without a principal, an UNRESERVE_RESOURCE for ANY object is also added
  // (see MESOS-9562).
  //
  // NOTE: Since the UNRESERVE operation only "pops" one reservation off the
  // stack of reservations, only principals of the most refined reservations
  // (i.e. ones that will be unreserved) are used.
  //
  // NOTE: Currently, validation of RESERVE operations prevents creating
  // reservation without a principal, so they should not exist in new
  // clusters.
  static void pushUnreserveActionObjects(
      const Resources& resources,
      std::vector<ActionObject>* result);
};

// Outputs human-readable description of an ActionObject.
//
// NOTE: For more convenient use in authorization-related messages
// ("Authorizing principal 'baz' to launch task", "Forbidden to create disk...",
// and so on), the description starts with a verb.
//
// Output examples:
//  - "launch task 123 of framework de-adbe-ef",
//  - "perform action FOO_BAR on object '{task_info: ..., machine_id: ...}"
std::ostream& operator<<(
    std::ostream& stream,
    const ActionObject& actionObject);


} // namespace authorization {
} // namespace mesos {

#endif // __MASTER_AUTHORIZATION_HPP__
