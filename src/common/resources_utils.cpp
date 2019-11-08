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

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "common/resources_utils.hpp"

using std::vector;

using google::protobuf::Descriptor;
using google::protobuf::Message;
using google::protobuf::RepeatedPtrField;

namespace mesos {

bool needCheckpointing(const Resource& resource)
{
  return !Resources::hasResourceProvider(resource) &&
         (Resources::isDynamicallyReserved(resource) ||
          Resources::isPersistentVolume(resource));
}


// NOTE: We effectively duplicate the logic in 'Resources::apply'
// which is less than ideal. But we cannot simply create
// 'Offer::Operation' and invoke 'Resources::apply' here.
// 'RESERVE' operation requires that the specified resources are
// dynamically reserved only, and 'CREATE' requires that the
// specified resources are already dynamically reserved.
// These requirements are violated when we try to infer dynamically
// reserved persistent volumes.
// TODO(mpark): Consider introducing an atomic 'RESERVE_AND_CREATE'
// operation to solve this problem.
Try<Resources> applyCheckpointedResources(
    const Resources& resources,
    const Resources& checkpointedResources)
{
  Resources totalResources = resources;

  foreach (const Resource& resource, checkpointedResources) {
    if (!needCheckpointing(resource)) {
      return Error("Unexpected checkpointed resources " + stringify(resource));
    }

    Resource stripped = resource;

    // Since only unreserved and statically reserved resources can be specified
    // on the agent, we strip away all of the dynamic reservations here to
    // deduce the agent resources on which to apply the checkpointed resources.
    if (Resources::isDynamicallyReserved(resource)) {
      Resource::ReservationInfo reservation = stripped.reservations(0);
      stripped.clear_reservations();
      if (reservation.type() == Resource::ReservationInfo::STATIC) {
        stripped.add_reservations()->CopyFrom(reservation);
      }
    }

    // Strip persistence and volume from the disk info so that we can
    // check whether it is contained in the `totalResources`.
    if (Resources::isPersistentVolume(resource)) {
      if (stripped.disk().has_source()) {
        stripped.mutable_disk()->clear_persistence();
        stripped.mutable_disk()->clear_volume();
      } else {
        stripped.clear_disk();
      }
    }

    stripped.clear_shared();

    if (!totalResources.contains(stripped)) {
      return Error(
          "Incompatible agent resources: " + stringify(totalResources) +
          " does not contain " + stringify(stripped));
    }

    totalResources -= stripped;
    totalResources += resource;
  }

  return totalResources;
}


namespace internal {

// NOTE: Use template here so that it works for both internal and v1.
template <typename TResources,
          typename TResource,
          typename TResourceConversion,
          typename TOperation>
Try<vector<TResourceConversion>> getResourceConversions(
    const TOperation& operation)
{
  vector<TResourceConversion> conversions;

  switch (operation.type()) {
    case TOperation::UNKNOWN:
      return Error("Unknown operation");

    case TOperation::LAUNCH:
    case TOperation::LAUNCH_GROUP:
    case TOperation::CREATE_DISK:
    case TOperation::DESTROY_DISK:
      return Error("Operation not supported");

    case TOperation::RESERVE: {
      TResources reserved(operation.reserve().resources());

      // If the operation explicitly specifies a `source` we use that,
      // otherwise we assume that the operation is "pushing" a single
      // reservation. At this point, the resources in the operation
      // should have been already sanity checked, so we don't have to
      // repeat that here.
      TResources consumed;
      if (operation.reserve().source_size() > 0) {
        consumed = TResources(operation.reserve().source());
      } else {
        consumed = reserved.popReservation();
      }
      conversions.emplace_back(consumed, reserved);
      break;
    }

    case TOperation::UNRESERVE: {
      foreach (const TResource& reserved, operation.unreserve().resources()) {
        // Note that we only allow "popping" a single reservation at time.
        TResources converted = TResources(reserved).popReservation();
        conversions.emplace_back(reserved, converted);
      }
      break;
    }

    case TOperation::CREATE: {
      foreach (const TResource& volume, operation.create().volumes()) {
        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        // TODO(jieyu): Non-persistent volumes are not supported for
        // now. Persistent volumes can only be be created from regular
        // disk resources. Revisit this once we start to support
        // non-persistent volumes.
        TResource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, the
        // original resource must be non-shared.
        stripped.clear_shared();

        conversions.emplace_back(stripped, volume);
      }
      break;
    }

    case TOperation::DESTROY: {
      foreach (const TResource& volume, operation.destroy().volumes()) {
        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        TResource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, we
        // return the resource to non-shared state after destroy.
        stripped.clear_shared();

        conversions.emplace_back(
            volume,
            stripped,
            [volume](const TResources& resources) -> Try<Nothing> {
              if (resources.contains(volume)) {
                return Error(
                  "Persistent volume " + stringify(volume) + " cannot be "
                  "removed due to additional shared copies");
              }
              return Nothing();
            });
      }
      break;
    }

    case TOperation::GROW_VOLUME: {
      const TResource& volume = operation.grow_volume().volume();
      const TResource& addition = operation.grow_volume().addition();

      if (TResources::hasResourceProvider(volume)) {
        return Error("Operation not supported for resource provider");
      }

      // To grow a persistent volume, we consume the original volume and the
      // additional resource and convert into a single volume with the new size.
      TResource converted = volume;
      *converted.mutable_scalar() += addition.scalar();

      conversions.emplace_back(TResources(volume) + addition, converted);
      break;
    }

    case TOperation::SHRINK_VOLUME: {
      const TResource& volume = operation.shrink_volume().volume();

      if (TResources::hasResourceProvider(volume)) {
        return Error("Operation not supported for resource provider");
      }

      // To shrink a persistent volume, we consume the original volume and
      // convert into a new volume with reduced size and a freed disk resource
      // without persistent volume info.
      TResource freed = volume;

      *freed.mutable_scalar() = operation.shrink_volume().subtract();

      // TODO(zhitao): Move this to helper function
      // `Resources::stripPersistentVolume`.
      if (freed.disk().has_source()) {
        freed.mutable_disk()->clear_persistence();
        freed.mutable_disk()->clear_volume();
      } else {
        freed.clear_disk();
      }

      // Since we only allow persistent volumes to be shared, the
      // freed resource must be non-shared.
      freed.clear_shared();

      TResource shrunk = volume;
      *shrunk.mutable_scalar() -= operation.shrink_volume().subtract();

      conversions.emplace_back(volume, TResources(shrunk) + freed);
      break;
    }
  }

  return conversions;
}

} // namespace internal {


Try<vector<ResourceConversion>> getResourceConversions(
    const Offer::Operation& operation)
{
  return internal::getResourceConversions<
      Resources,
      Resource,
      ResourceConversion,
      Offer::Operation>(operation);
}


Try<vector<v1::ResourceConversion>> getResourceConversions(
    const v1::Offer::Operation& operation)
{
  return internal::getResourceConversions<
      v1::Resources,
      v1::Resource,
      v1::ResourceConversion,
      v1::Offer::Operation>(operation);
}


Result<ResourceProviderID> getResourceProviderId(
    const Offer::Operation& operation)
{
  Option<Resource> resource;

  switch (operation.type()) {
    case Offer::Operation::LAUNCH:
      return Error("Unexpected LAUNCH operation");
    case Offer::Operation::LAUNCH_GROUP:
      return Error("Unexpected LAUNCH_GROUP operation");
    case Offer::Operation::RESERVE:
      if (operation.reserve().resources().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.reserve().resources(0);
      break;
    case Offer::Operation::UNRESERVE:
      if (operation.unreserve().resources().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.unreserve().resources(0);
      break;
    case Offer::Operation::CREATE:
      if (operation.create().volumes().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.create().volumes(0);
      break;
    case Offer::Operation::DESTROY:
      if (operation.destroy().volumes().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.destroy().volumes(0);
      break;
    case Offer::Operation::GROW_VOLUME:
      resource = operation.grow_volume().volume();
      break;
    case Offer::Operation::SHRINK_VOLUME:
      resource = operation.shrink_volume().volume();
      break;
    case Offer::Operation::CREATE_DISK:
      resource = operation.create_disk().source();
      break;
    case Offer::Operation::DESTROY_DISK:
      resource = operation.destroy_disk().source();
      break;
    case Offer::Operation::UNKNOWN:
      return Error("Unknown offer operation");
  }

  CHECK_SOME(resource);

  if (resource->has_provider_id()) {
    return resource->provider_id();
  }

  return None();
}


Result<ResourceProviderID> getResourceProviderId(const Resources& resources)
{
  if (resources.empty()) {
    return Error("Cannot determine resource provider for empty resources");
  }

  const Resource resource = *resources.begin();
  const Option<ResourceProviderID> resourceProviderId =
    resource.has_provider_id() ? resource.provider_id()
                               : Option<ResourceProviderID>::none();

  foreach (const Resource& resource_, resources) {
    const Option<ResourceProviderID> resourceProviderId_ =
      resource_.has_provider_id() ? resource_.provider_id()
                                  : Option<ResourceProviderID>::none();

    if (resourceProviderId_ != resourceProviderId) {
      return Error("Resources are from multiple resource providers");
    }
  }

  return resourceProviderId;
}


void convertResourceFormat(Resource* resource, ResourceFormat format)
{
  switch (format) {
    case PRE_RESERVATION_REFINEMENT:
    case ENDPOINT: {
      CHECK(!resource->has_role());
      CHECK(!resource->has_reservation());
      switch (resource->reservations_size()) {
        // Unreserved resource.
        case 0: {
          resource->set_role("*");
          break;
        }
        // Resource with a single reservation.
        case 1: {
          const Resource::ReservationInfo& source = resource->reservations(0);

          if (source.type() == Resource::ReservationInfo::DYNAMIC) {
            Resource::ReservationInfo* target = resource->mutable_reservation();
            if (source.has_principal()) {
              target->set_principal(source.principal());
            }
            if (source.has_labels()) {
              target->mutable_labels()->CopyFrom(source.labels());
            }
          }

          resource->set_role(source.role());

          if (format == PRE_RESERVATION_REFINEMENT) {
            resource->clear_reservations();
          }
          break;
        }
        // Resource with refined reservations.
        default: {
          CHECK_NE(PRE_RESERVATION_REFINEMENT, format)
            << "Invalid resource format conversion: A 'Resource' object"
               " being converted to the PRE_RESERVATION_REFINEMENT format"
               " must not have refined reservations";
        }
      }
      break;
    }
    case POST_RESERVATION_REFINEMENT: {
      if (resource->reservations_size() > 0) {
        // In this case, we're either already in
        // the "post-reservation-refinement" format,
        // or we're in the "endpoint" format.

        // We clear out the "pre-reservation-refinement" fields
        // in case the resources are in the "endpoint" format.
        resource->clear_role();
        resource->clear_reservation();
        return;
      }

      // Unreserved resources.
      if (resource->role() == "*") {
        CHECK(!resource->has_reservation());
        resource->clear_role();
        return;
      }

      // Resource with a single reservation.
      Resource::ReservationInfo* reservation = resource->add_reservations();

      // Check the `Resource.reservation` to determine whether
      // we have a static or dynamic reservation.
      if (!resource->has_reservation()) {
        reservation->set_type(Resource::ReservationInfo::STATIC);
      } else {
        reservation->CopyFrom(resource->reservation());
        resource->clear_reservation();
        reservation->set_type(Resource::ReservationInfo::DYNAMIC);
      }

      reservation->set_role(resource->role());
      resource->clear_role();
      break;
    }
  }
}


void convertResourceFormat(
    RepeatedPtrField<Resource>* resources,
    ResourceFormat format)
{
  foreach (Resource& resource, *resources) {
    convertResourceFormat(&resource, format);
  }
}


void convertResourceFormat(
    std::vector<Resource>* resources,
    ResourceFormat format)
{
  foreach (Resource& resource, *resources) {
    convertResourceFormat(&resource, format);
  }
}


namespace internal {

// Given a protobuf descriptor `descriptor`, recursively populates the provided
// `result` where the keys are the message descriptors within `descriptor`'s
// schema (including itself), and the corresponding value is `true` if the key
// contains a `mesos::Resource`, and `false` otherwise.
static void precomputeResourcesContainment(
    const Descriptor* descriptor,
    hashmap<const Descriptor*, bool>* result)
{
  CHECK_NOTNULL(descriptor);
  CHECK_NOTNULL(result);

  if (result->contains(descriptor)) {
    return;
  }

  if (descriptor == mesos::Resource::descriptor()) {
    result->insert({descriptor, true});
  }

  result->insert({descriptor, false});
  for (int i = 0; i < descriptor->field_count(); ++i) {
    // `message_type()` returns `nullptr` if the field is not a message type.
    const Descriptor* messageDescriptor = descriptor->field(i)->message_type();
    if (messageDescriptor == nullptr) {
      continue;
    }
    precomputeResourcesContainment(messageDescriptor, result);
    result->at(descriptor) |= result->at(messageDescriptor);
  }
}


static Try<Nothing> convertResourcesImpl(
    Message* message,
    Try<Nothing> (*convertResource)(mesos::Resource* resource),
    const hashmap<const Descriptor*, bool>& resourcesContainment)
{
  CHECK_NOTNULL(message);

  const Descriptor* descriptor = message->GetDescriptor();

  if (descriptor == mesos::Resource::descriptor()) {
    return convertResource(static_cast<mesos::Resource*>(message));
  }

  const google::protobuf::Reflection* reflection = message->GetReflection();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);
    const Descriptor* messageDescriptor = field->message_type();

    if (messageDescriptor == nullptr ||
        !resourcesContainment.at(messageDescriptor)) {
      continue;
    }

    if (!field->is_repeated()) {
      if (reflection->HasField(*message, field)) {
        Try<Nothing> result = convertResourcesImpl(
            reflection->MutableMessage(message, field),
            convertResource,
            resourcesContainment);

        if (result.isError()) {
          return result;
        }
      }
    } else {
      const int size = reflection->FieldSize(*message, field);

      for (int j = 0; j < size; ++j) {
        Try<Nothing> result = convertResourcesImpl(
            reflection->MutableRepeatedMessage(message, field, j),
            convertResource,
            resourcesContainment);

        if (result.isError()) {
          return result;
        }
      }
    }
  }

  return Nothing();
}

}  // namespace internal {


void upgradeResource(Resource* resource)
{
  convertResourceFormat(resource, POST_RESERVATION_REFINEMENT);
}


void upgradeResources(RepeatedPtrField<Resource>* resources)
{
  convertResourceFormat(resources, POST_RESERVATION_REFINEMENT);
}


void upgradeResources(vector<Resource>* resources)
{
  convertResourceFormat(resources, POST_RESERVATION_REFINEMENT);
}


void upgradeResources(Message* message)
{
  CHECK_NOTNULL(message);

  const Descriptor* descriptor = message->GetDescriptor();

  hashmap<const Descriptor*, bool> resourcesContainment;
  internal::precomputeResourcesContainment(descriptor, &resourcesContainment);

  if (!resourcesContainment.at(descriptor)) {
    return;
  }

  internal::convertResourcesImpl(
      message,
      [](Resource* resource) -> Try<Nothing> {
        upgradeResource(resource);
        return Nothing();
      },
      resourcesContainment);
}


Option<Error> validateAndUpgradeResources(Offer::Operation* operation)
{
  CHECK_NOTNULL(operation);

  switch (operation->type()) {
    case Offer::Operation::RESERVE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_reserve()) {
        return Error(
            "A RESERVE offer operation must have"
            " the Offer.Operation.reserve field set.");
      }

      Option<Error> error =
        Resources::validate(operation->reserve().resources());

      if (error.isSome()) {
        return error;
      }

      error = Resources::validate(operation->reserve().source());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::UNRESERVE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_unreserve()) {
        return Error(
            "An UNRESERVE offer operation must have"
            " the Offer.Operation.unreserve field set.");
      }

      Option<Error> error =
        Resources::validate(operation->unreserve().resources());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::CREATE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_create()) {
        return Error(
            "A CREATE offer operation must have"
            " the Offer.Operation.create field set.");
      }

      Option<Error> error =
        Resources::validate(operation->create().volumes());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::DESTROY: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_destroy()) {
        return Error(
            "A DESTROY offer operation must have"
            " the Offer.Operation.destroy field set.");
      }

      Option<Error> error =
        Resources::validate(operation->destroy().volumes());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::GROW_VOLUME: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_grow_volume()) {
        return Error(
            "A GROW_VOLUME operation must have"
            " the Offer.Operation.grow_volume field set");
      }

      Option<Error> error = Resources::validate(
          operation->grow_volume().volume());

      if (error.isSome()) {
        return error;
      }

      error = Resources::validate(operation->grow_volume().addition());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::SHRINK_VOLUME: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_shrink_volume()) {
        return Error(
            "A SHRINK_VOLUME offer operation must have"
            " the Offer.Operation.shrink_volume field set");
      }

      Option<Error> error = Resources::validate(
          operation->shrink_volume().volume());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::LAUNCH: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_launch()) {
        return Error(
            "A LAUNCH offer operation must have"
            " the Offer.Operation.launch field set.");
      }

      // Validate resources in LAUNCH.
      foreach (const TaskInfo& task, operation->launch().task_infos()) {
        Option<Error> error = Resources::validate(task.resources());
        if (error.isSome()) {
          return error;
        }

        if (task.has_executor()) {
          Option<Error> error =
            Resources::validate(task.executor().resources());

          if (error.isSome()) {
            return error;
          }
        }
      }

      break;
    }
    case Offer::Operation::LAUNCH_GROUP: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_launch_group()) {
        return Error(
            "A LAUNCH_GROUP offer operation must have"
            " the Offer.Operation.launch_group field set.");
      }

      Offer::Operation::LaunchGroup* launchGroup =
        operation->mutable_launch_group();

      // Validate resources in LAUNCH_GROUP.
      if (launchGroup->has_executor()) {
        Option<Error> error =
          Resources::validate(launchGroup->executor().resources());

        if (error.isSome()) {
          return error;
        }
      }

      foreach (const TaskInfo& task, launchGroup->task_group().tasks()) {
        Option<Error> error = Resources::validate(task.resources());
        if (error.isSome()) {
          return error;
        }

        if (task.has_executor()) {
          Option<Error> error =
            Resources::validate(task.executor().resources());

          if (error.isSome()) {
            return error;
          }
        }
      }

      break;
    }
    case Offer::Operation::CREATE_DISK: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_create_disk()) {
        return Error(
            "A CREATE_DISK offer operation must have"
            " the Offer.Operation.create_disk field set.");
      }

      Option<Error> error =
        Resources::validate(operation->create_disk().source());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::DESTROY_DISK: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_destroy_disk()) {
        return Error(
            "A DESTROY_DISK offer operation must have"
            " the Offer.Operation.destroy_disk field set.");
      }

      Option<Error> error =
        Resources::validate(operation->destroy_disk().source());

      if (error.isSome()) {
        return error;
      }

      break;
    }
    case Offer::Operation::UNKNOWN: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      return Error("Unknown offer operation");
    }
  }

  upgradeResources(operation);

  return None();
}


Try<Nothing> downgradeResource(Resource* resource)
{
  CHECK(!resource->has_role());
  CHECK(!resource->has_reservation());

  if (Resources::hasRefinedReservations(*resource)) {
    return Error("Cannot downgrade resources containing refined reservations");
  }

  convertResourceFormat(resource, PRE_RESERVATION_REFINEMENT);
  return Nothing();
}


Try<Nothing> downgradeResources(RepeatedPtrField<Resource>* resources)
{
  CHECK_NOTNULL(resources);

  foreach (Resource& resource, *resources) {
    Try<Nothing> result = downgradeResource(&resource);
    if (result.isError()) {
      return result;
    }
  }

  return Nothing();
}


Try<Nothing> downgradeResources(vector<Resource>* resources)
{
  CHECK_NOTNULL(resources);

  foreach (Resource& resource, *resources) {
    Try<Nothing> result = downgradeResource(&resource);
    if (result.isError()) {
      return result;
    }
  }

  return Nothing();
}


Try<Nothing> downgradeResources(Message* message)
{
  CHECK_NOTNULL(message);

  const Descriptor* descriptor = message->GetDescriptor();

  hashmap<const Descriptor*, bool> resourcesContainment;
  internal::precomputeResourcesContainment(descriptor, &resourcesContainment);

  if (!resourcesContainment.at(descriptor)) {
    return Nothing();
  }

  return internal::convertResourcesImpl(
      message, downgradeResource, resourcesContainment);
}


Resources shrinkResources(const Resources& resources, ResourceQuantities target)
{
  if (target.empty()) {
    return Resources();
  }

  // TODO(mzhu): Add a `shuffle()` method in `Resources` to avoid this copy.
  google::protobuf::RepeatedPtrField<Resource> resourceVector = resources;

  std::random_shuffle(resourceVector.begin(), resourceVector.end());

  Resources result;
  foreach (Resource& resource, resourceVector) {
    Value::Scalar scalar = target.get(resource.name());

    if (scalar == Value::Scalar()) {
      // Resource that has zero quantity is dropped (shrunk to zero).
      continue;
    }

    // Target can only be explicitly specified for scalar resources.
    CHECK_EQ(Value::SCALAR, resource.type()) << " Resources: " << resources;

    if (Resources::shrink(&resource, scalar)) {
      target -= ResourceQuantities::fromScalarResource(resource);
      result += std::move(resource);
    }
  }

  return result;
}


Resources shrinkResources(const Resources& resources, ResourceLimits target)
{
  if (target.empty()) {
    return resources;
  }

  // TODO(mzhu): Add a `shuffle()` method in `Resources` to avoid this copy.
  google::protobuf::RepeatedPtrField<Resource> resourceVector = resources;

  std::random_shuffle(resourceVector.begin(), resourceVector.end());

  Resources result;
  foreach (Resource resource, resourceVector) {
    Option<Value::Scalar> limit = target.get(resource.name());

    if (limit.isNone()) {
      // Resource that has infinite limit is kept as is.
      result += std::move(resource);
      continue;
    }

    // Target can only be explicitly specified for scalar resources.
    CHECK_EQ(Value::SCALAR, resource.type()) << " Resources: " << resources;

    if (Resources::shrink(&resource, *limit)) {
      target -= ResourceQuantities::fromScalarResource(resource);
      result += std::move(resource);
    }
  }

  return result;
}


} // namespace mesos {
