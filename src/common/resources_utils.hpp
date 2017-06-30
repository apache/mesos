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

#ifndef __RESOURCES_UTILS_HPP__
#define __RESOURCES_UTILS_HPP__

#include <vector>

#include <google/protobuf/repeated_field.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {

// Tests if the given Resource needs to be checkpointed on the slave.
// NOTE: We assume the given resource is validated.
bool needCheckpointing(const Resource& resource);

// Returns the total resources by applying the given checkpointed
// resources to the given resources. This function is useful when we
// want to calculate the total resources of a slave from the resources
// specified from the command line and the checkpointed resources.
// Returns error if the given resources are not compatible with the
// given checkpointed resources.
Try<Resources> applyCheckpointedResources(
    const Resources& resources,
    const Resources& checkpointedResources);


// Resource format options to be used with the `convertResourceFormat` function.
//
// The preconditions of the options are asymmetric, centered around the
// "post-reservation-refinement" format. This is mainly due to the fact that
// "post-reservation-refinement" format is our canonical representation.
// The transformations are generally applied to any of the 3 formats to be
// converted to the canonical format, then later converted back as necessary.
//
// See 'Resource Format' section in `mesos.proto` for more details.
enum ResourceFormat
{
  // "post-reservation-refinement" -> "pre-reservation-refinement"
  //
  // The `Resource` objects must be in the "post-reservation-refinement" format,
  // and must not have refined reservations.
  //
  // All resources end up with the `Resource.role` and `Resource.reservation`
  // fields set, and the `Resource.reservations` field unset.
  //
  // We convert the resources to the "pre-reservation-refinement" format to
  // checkpoint resources for example. This enables downgrading to an agent
  // without a RESERVATION_REFINEMENT caapbility, since the resources will
  // be checkpointed in a format that the downgraded agent can recover from.
  PRE_RESERVATION_REFINEMENT,

  // "pre-reservation-refinement"  -> "post-reservation-refinement"
  // "post-reservation-refinement" -> "post-reservation-refinement"
  // "endpoint"                    -> "post-reservation-refinement"
  //
  // The `Resource` objects can be in any of the valid resource formats:
  // "pre-reservation-refinement", "post-reservation-refinement", "endpoint".
  //
  // All resources end up with the `Resource.reservations` field set,
  // and the `Resource.role` and `Resource.reservation` fields unset.
  //
  // If the `Resource` objects are already in the "post-reservation-refinement"
  // format, this is a no-op.
  //
  // We convert the resources to the "post-reservation-refinement" format,
  // for example, when a master receives a message from an agent without
  // the RESERVATION_REFINEMENT capability. This allows a component
  // (e.g. master) code to deal with a canonical format to simplify the code.
  POST_RESERVATION_REFINEMENT,

  // "post-reservation-refinement" -> "endpoint"
  //
  // This is a special case for endpoints, which injects
  // the "pre-reservation-refinement" format.
  //
  // The `Resource` objects must be in the "post-reservation-refinement" format.
  //
  // All resources continue to have the `Resource.reservations` field set.
  // The `Resource` objects without refined reservations end up with the
  // `Resource.role` and `Resource.reservation` fields set, and the objects
  // with refined reservations have them unset.
  //
  // We inject the resources with the "pre-reservation-refinement" format to
  // enable backward compatibility with external tooling. If the master has been
  // upgraded to a version that supports reservation refinement but no refined
  // reservations have been made, the endpoints will return the data in both new
  // and old formats to maximize backward compatibility. However, once
  // a reservation refinement is made to a resource, that resource is only
  // returned in the new format.
  ENDPOINT
};


// Converts the given `Resource` to the specified `ResourceFormat`.
//
// See the "Resource Format" section in `mesos.proto` for more details.
// See the `ResourceFormat` enum above for more details.
void convertResourceFormat(Resource* resource, ResourceFormat format);


// Converts the given `Resource`s to the specified `ResourceFormat`.
void convertResourceFormat(
    google::protobuf::RepeatedPtrField<Resource>* resources,
    ResourceFormat format);


// Converts the given `Resource`s to the specified `ResourceFormat`.
void convertResourceFormat(
    std::vector<Resource>* resources,
    ResourceFormat format);


// Convert the resources in the given `Operation` to the
// "post-reservation-refinement" format from any format
// ("pre-", "post-" or "endpoint") if all of the resources are valid.
// Returns an `Error` if there are any invalid resources present;
// in this case, all resources are left unchanged.
// NOTE: The validate and upgrade steps are bundled because currently
// it would be an error to validate but not upgrade or to upgrade
// without validating.
Option<Error> validateAndNormalizeResources(Offer::Operation* operation);


// Convert the given resources to the "pre-reservation-refinement" format
// if none of the resources have refined reservations. Returns an `Error`
// if there are any refined reservations present; in this case, the resources
// are left in the "post-reservation-refinement" format.
Try<Nothing> downgradeResources(
    google::protobuf::RepeatedPtrField<Resource>* resources);

} // namespace mesos {

#endif // __RESOURCES_UTILS_HPP__
