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

#include <stout/protobuf.hpp>
#include "common/protobuf_utils.hpp"

#include "master/authorization.hpp"

using std::ostream;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace authorization {


ActionObject ActionObject::fromResourceWithLegacyValue(
    const Action action,
    const Resource& resource,
    string value)
{
  Object object;
  *object.mutable_resource() = resource;
  *object.mutable_value() = std::move(value);

  return ActionObject(action, std::move(object));
}


ActionObject ActionObject::taskLaunch(
    const TaskInfo& task,
    const FrameworkInfo& framework)
{
  Object object;
  *object.mutable_task_info() = task;
  *object.mutable_framework_info() = framework;

  return ActionObject(authorization::RUN_TASK, std::move(object));
}


ActionObject ActionObject::frameworkRegistration(
    const FrameworkInfo& frameworkInfo)
{
  Object object;
  *object.mutable_framework_info() = frameworkInfo;

  // For non-`MULTI_ROLE` frameworks, also propagate its single role
  // via the request's `value` field. This is purely for backwards
  // compatibility as the `value` field is deprecated. Note that this
  // means that authorizers relying on the deprecated field will see
  // an empty string in `value` for `MULTI_ROLE` frameworks.
  //
  // TODO(bbannier): Remove this at the end of `value`'s deprecation
  // cycle, see MESOS-7073.
  if (!::mesos::internal::protobuf::frameworkHasCapability(
          frameworkInfo, FrameworkInfo::Capability::MULTI_ROLE)) {
    object.set_value(frameworkInfo.role());
  }

  return ActionObject(authorization::REGISTER_FRAMEWORK, std::move(object));
}


vector<ActionObject> ActionObject::agentRegistration(
    const SlaveInfo& slaveInfo)
{
  vector<ActionObject> result;

  if (!Resources(slaveInfo.resources()).reserved().empty()) {
    // If static reservations exist, we also need to authorize them.
    //
    // NOTE: We don't look at dynamic reservations in checkpointed
    // resources because they should have gone through authorization
    // against the framework / operator's principal when they were
    // created. In constrast, static reservations are initiated by the
    // agent's principal and authorizing them helps prevent agents from
    // advertising reserved resources of arbitrary roles.
    Offer::Operation::Reserve reserve;
    *reserve.mutable_resources() = slaveInfo.resources();
    result = ActionObject::reserve(reserve);
  }

  result.push_back(ActionObject(authorization::REGISTER_AGENT, None()));
  return result;
}


// Returns effective reservation role of resource for the purpose
// of authorizing an operation; that is, the the role of the most
// refined reservation if the resource is reserved.
//
// NOTE: If the resource is not reserved, the default role '*' is
// returned; this is needed to handle authorization of invalid
// operations and could be avoided if we were able to guarantee
// that authorization Objects for invalid operations are never built
// (see MESOS-10083).
//
// NOTE: If the resource is not in the post-reservation-refinement
// format, this function will crash the program.
string getReservationRole(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return Resources::isReserved(resource)
    ? Resources::reservationRole(resource) : "*";
}


void ActionObject::pushUnreserveActionObjects(
    const Resources& resources,
    vector<ActionObject>* result)
{
  bool hasReservationsWithoutPrincipal = false;

  for (const Resource& resource : resources) {
    // NOTE: We rely on the master to ensure that the resource is in the
    // post-reservation-refinement format.
    CHECK(!resource.has_role()) << resource;
    CHECK(!resource.has_reservation()) << resource;

    if (!resource.reservations().empty() &&
        resource.reservations().rbegin()->has_principal()) {
      result->push_back(fromResourceWithLegacyValue(
          authorization::UNRESERVE_RESOURCES,
          resource,
          resource.reservations().rbegin()->principal()));
    } else {
      hasReservationsWithoutPrincipal = true;
    }
  }

  if (hasReservationsWithoutPrincipal) {
    result->push_back(ActionObject(authorization::UNRESERVE_RESOURCES, None()));
  }
}


vector<ActionObject>
ActionObject::unreserve(const Offer::Operation::Unreserve& unreserve)
{
  vector<ActionObject> result;
  pushUnreserveActionObjects(unreserve.resources(), &result);
  // NOTE: In some cases, such as UNRESERVE with no resources
  // (which is invalid) or resources reserved without a principal
  // by an old version of Mesos, an empty vector will be returned.
  return result;
}


vector<ActionObject> ActionObject::reserve(
    const Offer::Operation::Reserve& reserve)
{
  std::vector<ActionObject> result;
  auto pushReserveActionObjects =
    [&result](const Resources& resources) mutable {
      // The operation will be authorized if the entity is allowed to make
      // reservations for all unique roles included in `resources`.
      //
      // TODO(asekretenko): This approach assumes that resources with identical
      // reservation roles are equivalent from the authorizers' point of view;
      // in future this assumption might become incorrect. As there is no reason
      // other than performance to leave only resources with unique roles, this
      // filtration can probably be avoided after implementing synchronous
      // authorization (see MESOS-10056).
      hashset<string> roles;

      foreach (const Resource& resource, resources) {
        const string role = getReservationRole(resource);

        if (!roles.contains(role)) {
          roles.insert(role);
          result.push_back(fromResourceWithLegacyValue(
            authorization::RESERVE_RESOURCES, resource, role));
        }
      }
    };

  // We support `Reserve` operations with either `source` set or unset. If
  // `source` is unset (an older API), it is possible to send calls which
  // contain also resources whose reservations are unmodified; in that case
  // all `Reserve` `resources` will be authorized. In the case where `source`
  // is set we follow a narrower contract and e.g., only accept resources
  // whose reservations are all modified, and require identical modifications
  // for all passed `Resource`s.
  //
  // This e.g., means that we cannot upgrade calls without `source` to
  // calls with `source` and use uniform handling. Instead we branch
  // on whether `source` was set to select the authorization handling.
  // Each fundamental reservation added in with with-`source` case can
  // then be authorized using the historic, wider contract.
  if (reserve.source().empty())
  {
    pushReserveActionObjects(reserve.resources());
    return result;
  }

  Resources source = reserve.source();
  const Resources target = reserve.resources();
  const Resources ancestor =
    Resources::getReservationAncestor(source, target);

  // We request UNRESERVE_RESOURCES to bring `source` to `ancestor`.
  while (source != ancestor) {
    pushUnreserveActionObjects(source, &result);
    source = source.popReservation();
  }

  // We request RESERVE_RESOURCES to bring `ancestor` to `target`.
  const RepeatedPtrField<Resource::ReservationInfo> targetReservations =
    reserve.resources(0).reservations();
  const RepeatedPtrField<Resource::ReservationInfo> ancestorReservations =
    RepeatedPtrField<Resource>(ancestor).begin()->reservations();

  // Skip reservations common among `source` and `resources`.
  auto it = targetReservations.begin();
  std::advance(it, ancestorReservations.size());

  for (; it != targetReservations.end(); ++it) {
    source = source.pushReservation(*it);
    pushReserveActionObjects(source);
  }

  // NOTE: In some cases, such as RESERVE with source identical to target,
  // an empty vector will be returned.
  return result;
}


vector<ActionObject> ActionObject::createVolume(
    const Offer::Operation::Create& create)
{
  vector<ActionObject> result;

  // The operation will be authorized if the entity is allowed
  // to create volumes for all roles included in `create.volumes`.

  // Add an object for each unique role in the volumes.
  hashset<string> roles;

  foreach (const Resource& volume, create.volumes()) {
    string role = getReservationRole(volume);

    if (!roles.contains(role)) {
      roles.insert(role);
      result.push_back(fromResourceWithLegacyValue(
          authorization::CREATE_VOLUME, volume, role));
    }
  }

  if (result.empty()) {
    result.push_back(ActionObject(authorization::CREATE_VOLUME, None()));
  }

  return result;
}


vector<ActionObject> ActionObject::destroyVolume(
    const Offer::Operation::Destroy& destroy)
{
  vector<ActionObject> result;

  // NOTE: As this code can be called for an invalid operation, we create
  // action-object pairs only for resources that are persistent volumes and
  // skip all the others, relying on the caller to perform validation after
  // authorization (see MESOS-10083).
  foreach (const Resource& volume, destroy.volumes()) {
    // TODO(asekretenko): Replace with CHECK after MESOS-10083 is fixed.
    if (volume.has_disk() && volume.disk().has_persistence()) {
      result.push_back(fromResourceWithLegacyValue(
          authorization::DESTROY_VOLUME,
          volume,
          volume.disk().persistence().principal()));
    }
  }

  if (result.empty()) {
    result.push_back(ActionObject(authorization::DESTROY_VOLUME, None()));
  }

  return result;
}


ActionObject ActionObject::growVolume(const Offer::Operation::GrowVolume& grow)
{
  return fromResourceWithLegacyValue(
      authorization::RESIZE_VOLUME,
      grow.volume(),
      getReservationRole(grow.volume()));
}


ActionObject ActionObject::shrinkVolume(
    const Offer::Operation::ShrinkVolume& shrink)
{
  return fromResourceWithLegacyValue(
      authorization::RESIZE_VOLUME,
      shrink.volume(),
      getReservationRole(shrink.volume()));
}


Try<ActionObject> ActionObject::createDisk(
    const Offer::Operation::CreateDisk& createDisk)
{
  auto action = ([&createDisk]() -> Option<authorization::Action> {
    switch (createDisk.target_type()) {
      case Resource::DiskInfo::Source::MOUNT:
        return authorization::CREATE_MOUNT_DISK;
      case Resource::DiskInfo::Source::BLOCK:
        return authorization::CREATE_BLOCK_DISK;
      case Resource::DiskInfo::Source::UNKNOWN:
      case Resource::DiskInfo::Source::PATH:
      case Resource::DiskInfo::Source::RAW:
        return None();
    }

    return None();
  })();

  if (action.isNone()) {
    return Error(
        "Unsupported disk type: " + stringify(createDisk.target_type()));
  }

  return fromResourceWithLegacyValue(
      *action, createDisk.source(), getReservationRole(createDisk.source()));
}


Try<ActionObject> ActionObject::destroyDisk(
    const Offer::Operation::DestroyDisk& destroyDisk)
{
  const Resource& resource = destroyDisk.source();

  auto action = ([&resource]() -> Option<authorization::Action> {
    switch (resource.disk().source().type()) {
      case Resource::DiskInfo::Source::MOUNT:
        return authorization::DESTROY_MOUNT_DISK;
      case Resource::DiskInfo::Source::BLOCK:
        return authorization::DESTROY_BLOCK_DISK;
      case Resource::DiskInfo::Source::RAW:
        return authorization::DESTROY_RAW_DISK;
      case Resource::DiskInfo::Source::UNKNOWN:
      case Resource::DiskInfo::Source::PATH:
        return None();
    }

    return None();
  })();

  if (action.isNone()) {
    return Error(
        "Unsupported disk type: " + stringify(resource.disk().source().type()));
  }

  return fromResourceWithLegacyValue(
      *action, resource, getReservationRole(resource));
}


ostream& operator<<(ostream& stream, const ActionObject& actionObject)
{
  const Option<Object>& object = actionObject.object();

  if (object.isNone()) {
    return stream << "perform action " << Action_Name(actionObject.action())
                  << " on ANY object";
  }

  switch (actionObject.action()) {
    case authorization::RUN_TASK: {
      const TaskInfo& task = object->task_info();
      const FrameworkInfo& framework = object->framework_info();
      return stream
        << "launch task " << task.task_id()
        << " of framework " << framework.id()
        << " under user '"
        << (task.has_command() && task.command().has_user()
            ? task.command().user()
            : task.has_executor() && task.executor().command().has_user()
              ? task.executor().command().user()
              : framework.user())
        << "'";
    }

    case authorization::REGISTER_FRAMEWORK:
      return stream
        << "register framework " << object->framework_info().id()
        << " with roles "
        << stringify(::mesos::internal::protobuf::framework::getRoles(
               object->framework_info()));

    default:
      break;
  }

  return stream
    << "perform action " << Action_Name(actionObject.action())
    << " on object " << jsonify(JSON::Protobuf(*object));
}


} // namespace authorization {
} // namespace mesos {
