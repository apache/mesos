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

#include <stdint.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <google/protobuf/repeated_field.h>

#include <mesos/resource_quantities.hpp>
#include <mesos/resources.hpp>
#include <mesos/roles.hpp>
#include <mesos/values.hpp>
#include <mesos/type_utils.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include "common/resources_utils.hpp"

using std::make_shared;
using std::map;
using std::ostream;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {

/////////////////////////////////////////////////
// Helper functions.
/////////////////////////////////////////////////

bool operator==(
    const Resource::AllocationInfo& left,
    const Resource::AllocationInfo& right)
{
  if (left.has_role() != right.has_role()) {
    return false;
  }

  if (left.has_role() && left.role() != right.role()) {
    return false;
  }

  return true;
}


bool operator!=(
    const Resource::AllocationInfo& left,
    const Resource::AllocationInfo& right)
{
  return !(left == right);
}


bool operator==(
    const Resource::ReservationInfo& left,
    const Resource::ReservationInfo& right)
{
  if (left.type() != right.type()) {
    return false;
  }

  if (left.role() != right.role()) {
    return false;
  }

  if (left.has_principal() != right.has_principal()) {
    return false;
  }

  if (left.has_principal() && left.principal() != right.principal()) {
    return false;
  }

  if (left.has_labels() != right.has_labels()) {
    return false;
  }

  if (left.has_labels() && left.labels() != right.labels()) {
    return false;
  }

  return true;
}


bool operator!=(
    const Resource::ReservationInfo& left,
    const Resource::ReservationInfo& right)
{
  return !(left == right);
}


bool operator==(
    const Resource::DiskInfo::Source::Path& left,
    const Resource::DiskInfo::Source::Path& right)
{
  if (left.has_root() != right.has_root()) {
    return false;
  }

  if (left.has_root() && left.root() != right.root()) {
    return false;
  }

  return true;
}


bool operator==(
    const Resource::DiskInfo::Source::Mount& left,
    const Resource::DiskInfo::Source::Mount& right)
{
  if (left.has_root() != right.has_root()) {
    return false;
  }

  if (left.has_root() && left.root() != right.root()) {
    return false;
  }

  return true;
}


bool operator!=(
    const Resource::DiskInfo::Source::Path& left,
    const Resource::DiskInfo::Source::Path& right)
{
  return !(left == right);
}


bool operator!=(
    const Resource::DiskInfo::Source::Mount& left,
    const Resource::DiskInfo::Source::Mount& right)
{
  return !(left == right);
}


bool operator==(
    const Resource::DiskInfo::Source& left,
    const Resource::DiskInfo::Source& right)
{
  if (left.type() != right.type()) {
    return false;
  }

  if (left.has_path() != right.has_path()) {
    return false;
  }

  if (left.has_path() && left.path() != right.path()) {
    return false;
  }

  if (left.has_mount() != right.has_mount()) {
    return false;
  }

  if (left.has_mount() && left.mount() != right.mount()) {
    return false;
  }

  if (left.has_vendor() != right.has_vendor()) {
    return false;
  }

  if (left.has_vendor() && left.vendor() != right.vendor()) {
    return false;
  }

  if (left.has_id() != right.has_id()) {
    return false;
  }

  if (left.has_id() && left.id() != right.id()) {
    return false;
  }

  if (left.has_metadata() != right.has_metadata()) {
    return false;
  }

  if (left.has_metadata() && left.metadata() != right.metadata()) {
    return false;
  }

  if (left.has_profile() != right.has_profile()) {
    return false;
  }

  if (left.has_profile() && left.profile() != right.profile()) {
    return false;
  }

  return true;
}


bool operator!=(
    const Resource::DiskInfo::Source& left,
    const Resource::DiskInfo::Source& right)
{
  return !(left == right);
}


bool operator==(const Resource::DiskInfo& left, const Resource::DiskInfo& right)
{
  if (left.has_source() != right.has_source()) {
    return false;
  }

  if (left.has_source() && left.source() != right.source()) {
    return false;
  }

  // NOTE: We ignore 'volume' inside DiskInfo when doing comparison
  // because it describes how this resource will be used which has
  // nothing to do with the Resource object itself. A framework can
  // use this resource and specify different 'volume' every time it
  // uses it.
  if (left.has_persistence() != right.has_persistence()) {
    return false;
  }

  if (left.has_persistence() &&
      left.persistence().id() != right.persistence().id()) {
    return false;
  }

  return true;
}


bool operator!=(const Resource::DiskInfo& left, const Resource::DiskInfo& right)
{
  return !(left == right);
}


static bool compareResourceMetadata(const Resource& left, const Resource& right)
{
  if (left.name() != right.name() || left.type() != right.type()) {
    return false;
  }

  // Check AllocationInfo.
  if (left.has_allocation_info() != right.has_allocation_info()) {
    return false;
  }

  if (left.has_allocation_info() &&
      left.allocation_info() != right.allocation_info()) {
    return false;
  }

  // Check the stack of ReservationInfo.
  if (left.reservations_size() != right.reservations_size()) {
    return false;
  }

  for (int i = 0; i < left.reservations_size(); ++i) {
    if (left.reservations(i) != right.reservations(i)) {
      return false;
    }
  }

  // Check DiskInfo.
  if (left.has_disk() != right.has_disk()) {
    return false;
  }

  if (left.has_disk() && left.disk() != right.disk()) {
    return false;
  }

  // Check RevocableInfo.
  if (left.has_revocable() != right.has_revocable()) {
    return false;
  }

  // Check ResourceProviderID.
  if (left.has_provider_id() != right.has_provider_id()) {
    return false;
  }

  if (left.has_provider_id() && left.provider_id() != right.provider_id()) {
    return false;
  }

  // Check SharedInfo.
  if (left.has_shared() != right.has_shared()) {
    return false;
  }

  return true;
}


bool operator==(const Resource& left, const Resource& right) {
  if (!compareResourceMetadata(left, right)) {
    return false;
  }

  if (left.type() == Value::SCALAR) {
    return left.scalar() == right.scalar();
  } else if (left.type() == Value::RANGES) {
    return left.ranges() == right.ranges();
  } else if (left.type() == Value::SET) {
    return left.set() == right.set();
  } else {
    return false;
  }
}


bool operator!=(const Resource& left, const Resource& right)
{
  return !(left == right);
}


namespace internal {

// Tests if we can add two Resource objects together resulting in one
// valid Resource object. For example, two Resource objects with
// different name, type or role are not addable.
static bool addable(const Resource& left, const Resource& right)
{
  // Check SharedInfo.
  if (left.has_shared() != right.has_shared()) {
    return false;
  }

  // For shared resources, they can be added only if left == right.
  if (left.has_shared()) {
    return left == right;
  }

  // Now, we verify if the two non-shared resources can be added.
  if (left.name() != right.name() || left.type() != right.type()) {
    return false;
  }

  // Check AllocationInfo.
  if (left.has_allocation_info() != right.has_allocation_info()) {
    return false;
  }

  if (left.has_allocation_info() &&
      left.allocation_info() != right.allocation_info()) {
    return false;
  }

  // Check the stack of ReservationInfo.
  if (left.reservations_size() != right.reservations_size()) {
    return false;
  }

  for (int i = 0; i < left.reservations_size(); ++i) {
    if (left.reservations(i) != right.reservations(i)) {
      return false;
    }
  }

  // Check DiskInfo.
  if (left.has_disk() != right.has_disk()) { return false; }

  if (left.has_disk()) {
    if (left.disk() != right.disk()) { return false; }

    if (left.disk().has_source()) {
      switch (left.disk().source().type()) {
        case Resource::DiskInfo::Source::PATH: {
          // Two PATH resources can be added if their disks are identical.
          break;
        }
        case Resource::DiskInfo::Source::BLOCK:
        case Resource::DiskInfo::Source::MOUNT: {
          // Two resources that represent exclusive 'MOUNT' or 'RAW' disks
          // cannot be added together; this would defeat the exclusivity.
          return false;
        }
        case Resource::DiskInfo::Source::RAW: {
          // We can only add resources representing 'RAW' disks if
          // they have no identity or are identical.
          if (left.disk().source().has_id()) {
            return false;
          }
          break;
        }
        case Resource::DiskInfo::Source::UNKNOWN:
          UNREACHABLE();
      }
    }

    // TODO(jieyu): Even if two Resource objects with DiskInfo have
    // the same persistence ID, they cannot be added together if they
    // are non-shared. In fact, this shouldn't happen if we do not
    // add resources from different namespaces (e.g., across slave).
    // Consider adding a warning.
    if (left.disk().has_persistence()) {
      return false;
    }
  }

  // Check RevocableInfo.
  if (left.has_revocable() != right.has_revocable()) {
    return false;
  }

  // Check ResourceProviderID.
  if (left.has_provider_id() != right.has_provider_id()) {
    return false;
  }

  if (left.has_provider_id() && left.provider_id() != right.provider_id()) {
    return false;
  }

  return true;
}


// Tests if we can subtract "right" from "left" resulting in one valid
// Resource object. For example, two Resource objects with different
// name, type or role are not subtractable.
// NOTE: Set subtraction is always well defined, it does not require
// 'right' to be contained within 'left'. For example, assuming that
// "left = {1, 2}" and "right = {2, 3}", "left" and "right" are
// subtractable because "left - right = {1}". However, "left" does not
// contain "right".
static bool subtractable(const Resource& left, const Resource& right)
{
  // Check SharedInfo.
  if (left.has_shared() != right.has_shared()) {
    return false;
  }

  // For shared resources, they can be subtracted only if left == right.
  if (left.has_shared()) {
    return left == right;
  }

  // Now, we verify if the two non-shared resources can be subtracted.
  if (left.name() != right.name() || left.type() != right.type()) {
    return false;
  }

  // Check AllocationInfo.
  if (left.has_allocation_info() != right.has_allocation_info()) {
    return false;
  }

  if (left.has_allocation_info() &&
      left.allocation_info() != right.allocation_info()) {
    return false;
  }

  // Check the stack of ReservationInfo.
  if (left.reservations_size() != right.reservations_size()) {
    return false;
  }

  for (int i = 0; i < left.reservations_size(); ++i) {
    if (left.reservations(i) != right.reservations(i)) {
      return false;
    }
  }

  // Check DiskInfo.
  if (left.has_disk() != right.has_disk()) { return false; }

  if (left.has_disk()) {
    if (left.disk() != right.disk()) { return false; }

    if (left.disk().has_source()) {
      switch (left.disk().source().type()) {
        case Resource::DiskInfo::Source::PATH: {
          // Two PATH resources can be subtracted if their disks are identical.
          break;
        }
        case Resource::DiskInfo::Source::BLOCK:
        case Resource::DiskInfo::Source::MOUNT: {
          // Two resources that represent exclusive 'MOUNT' or 'BLOCK' disks
          // cannot be subtracted from each other if they are not the exact same
          // mount; this would defeat the exclusivity.
          if (left != right) {
            return false;
          }
          break;
        }
        case Resource::DiskInfo::Source::RAW: {
          // We can only subtract resources representing 'RAW' disks
          // if they have no identity, or refer to the same disk.
          if (left.disk().source().has_id() && left != right) {
            return false;
          }
          break;
        }
        case Resource::DiskInfo::Source::UNKNOWN:
          UNREACHABLE();
      }
    }

    // NOTE: For Resource objects that have DiskInfo, we can only subtract
    // if they are equal.
    if (left.disk().has_persistence() && left != right) {
      return false;
    }
  }

  // Check RevocableInfo.
  if (left.has_revocable() != right.has_revocable()) {
    return false;
  }

  // Check ResourceProviderID.
  if (left.has_provider_id() != right.has_provider_id()) {
    return false;
  }

  if (left.has_provider_id() && left.provider_id() != right.provider_id()) {
    return false;
  }

  return true;
}


/**
 * Checks that a Resources object is valid for command line specification.
 *
 * Checks that the given Resources object is appropriate for specification at
 * the command line. Resources are appropriate if they do not have two resources
 * with the same name but different types, and do not attempt to specify
 * persistent volumes, revocable resources, or dynamic reservations.
 *
 * @param resources The input Resources.
 * @return An `Option` containing None() if validation was successful, or an
 *     Error otherwise.
 */
static Option<Error> validateCommandLineResources(const Resources& resources)
{
  hashmap<string, Value::Type> nameTypes;

  foreach (const Resource& resource, resources) {
    // These fields should only be provided programmatically,
    // not at the command line.
    if (Resources::isPersistentVolume(resource)) {
      return Error(
          "Persistent volumes cannot be specified at the command line");
    } else if (Resources::isRevocable(resource)) {
      return Error(
          "Revocable resources cannot be specified at the command line; do"
          " not include a 'revocable' key in the resources JSON");
    } else if (Resources::isDynamicallyReserved(resource)) {
      return Error(
          "Dynamic reservations cannot be specified at the command line; do"
          " not include a reservation with DYNAMIC type in the resources JSON");
    }

    if (nameTypes.contains(resource.name()) &&
        nameTypes[resource.name()] != resource.type()) {
      return Error(
          "Resources with the same name ('" + resource.name() + "') but"
          " different types are not allowed");
    } else if (!nameTypes.contains(resource.name())) {
      nameTypes[resource.name()] = resource.type();
    }
  }

  return None();
}

} // namespace internal {


Resource& operator+=(Resource& left, const Resource& right)
{
  if (left.type() == Value::SCALAR) {
    *left.mutable_scalar() += right.scalar();
  } else if (left.type() == Value::RANGES) {
    *left.mutable_ranges() += right.ranges();
  } else if (left.type() == Value::SET) {
    *left.mutable_set() += right.set();
  }

  return left;
}


Resource operator+(const Resource& left, const Resource& right)
{
  Resource result = left;
  result += right;
  return result;
}


Resource& operator-=(Resource& left, const Resource& right)
{
  if (left.type() == Value::SCALAR) {
    *left.mutable_scalar() -= right.scalar();
  } else if (left.type() == Value::RANGES) {
    *left.mutable_ranges() -= right.ranges();
  } else if (left.type() == Value::SET) {
    *left.mutable_set() -= right.set();
  }

  return left;
}


Resource operator-(const Resource& left, const Resource& right)
{
  Resource result = left;
  result -= right;
  return result;
}


/////////////////////////////////////////////////
// Public static functions.
/////////////////////////////////////////////////

Try<Resource> Resources::parse(
    const string& name,
    const string& value,
    const string& role)
{
  Try<Value> result = internal::values::parse(value);
  if (result.isError()) {
    return Error(
        "Failed to parse resource " + name +
        " value " + value + " error " + result.error());
  }

  Resource resource;

  Value _value = result.get();
  resource.set_name(name);

  if (role != "*") {
    Resource::ReservationInfo* reservation = resource.add_reservations();
    reservation->set_type(Resource::ReservationInfo::STATIC);
    reservation->set_role(role);
  }

  if (_value.type() == Value::SCALAR) {
    resource.set_type(Value::SCALAR);
    resource.mutable_scalar()->CopyFrom(_value.scalar());
  } else if (_value.type() == Value::RANGES) {
    resource.set_type(Value::RANGES);
    resource.mutable_ranges()->CopyFrom(_value.ranges());
  } else if (_value.type() == Value::SET) {
    resource.set_type(Value::SET);
    resource.mutable_set()->CopyFrom(_value.set());
  } else {
    return Error(
        "Bad type for resource " + name + " value " + value +
        " type " + Value::Type_Name(_value.type()));
  }

  return resource;
}


// TODO(wickman) It is possible for Resources::ostream<< to produce
// unparseable resources, i.e.  those with
// ReservationInfo/DiskInfo/RevocableInfo.
Try<Resources> Resources::parse(
    const string& text,
    const string& defaultRole)
{
  Try<vector<Resource>> resources = Resources::fromString(text, defaultRole);

  if (resources.isError()) {
    return Error(resources.error());
  }

  Resources result;

  // Validate the Resource objects.
  foreach (Resource& resource, CHECK_NOTERROR(resources)) {
    // If invalid, propgate error instead of skipping the resource.
    Option<Error> error = Resources::validate(resource);
    if (error.isSome()) {
      return error.get();
    }

    result.add(std::move(resource));
  }

  // TODO(jmlvanre): Move this up into `Containerizer::resources`.
  Option<Error> error = internal::validateCommandLineResources(result);
  if (error.isSome()) {
    return error.get();
  }

  return result;
}


Try<vector<Resource>> Resources::fromJSON(
    const JSON::Array& resourcesJSON,
    const string& defaultRole)
{
  // Convert the JSON Array into a protobuf message and use
  // that to construct a vector of Resource object.
  Try<RepeatedPtrField<Resource>> resourcesProtobuf =
    protobuf::parse<RepeatedPtrField<Resource>>(resourcesJSON);

  if (resourcesProtobuf.isError()) {
    return Error(
        "Some JSON resources were not formatted properly: " +
        resourcesProtobuf.error());
  }

  vector<Resource> result;

  foreach (Resource& resource, resourcesProtobuf.get()) {
    // Set the default role if none was specified.
    //
    // NOTE: We rely on the fact that the result of this function is
    // converted to the "post-reservation-refinement" format.
    if (!resource.has_role() && resource.reservations_size() == 0) {
      resource.set_role(defaultRole);
    }

    upgradeResource(&resource);

    // We add the Resource object even if it is empty or invalid.
    result.push_back(resource);
  }

  return result;
}


Try<vector<Resource>> Resources::fromSimpleString(
    const string& text,
    const string& defaultRole)
{
  vector<Resource> resources;

  foreach (const string& token, strings::tokenize(text, ";")) {
    // TODO(anindya_sinha): Allow text based representation of resources
    // to specify PATH or MOUNT type disks along with its root.
    vector<string> pair = strings::tokenize(token, ":");
    if (pair.size() != 2) {
      return Error(
          "Bad value for resources, missing or extra ':' in " + token);
    }

    string name;
    string role;
    size_t openParen = pair[0].find('(');
    if (openParen == string::npos) {
      name = strings::trim(pair[0]);
      role = defaultRole;
    } else {
      size_t closeParen = pair[0].find(')');
      if (closeParen == string::npos || closeParen < openParen) {
        return Error(
            "Bad value for resources, mismatched parentheses in " + token);
      }

      name = strings::trim(pair[0].substr(0, openParen));

      role = strings::trim(pair[0].substr(
          openParen + 1,
          closeParen - openParen - 1));
    }

    Try<Resource> resource = Resources::parse(name, pair[1], role);
    if (resource.isError()) {
      return Error(resource.error());
    }

    upgradeResource(&(resource.get()));

    // We add the Resource object even if it is empty or invalid.
    resources.push_back(resource.get());
  }

  return resources;
}


Try<vector<Resource>> Resources::fromString(
    const string& text,
    const string& defaultRole)
{
  // Try to parse as a JSON Array. Otherwise, parse as a text string.
  Try<JSON::Array> json = JSON::parse<JSON::Array>(text);

  return json.isSome() ?
    Resources::fromJSON(json.get(), defaultRole) :
    Resources::fromSimpleString(text, defaultRole);
}


Option<Error> Resources::validate(const Resource& resource)
{
  if (resource.name().empty()) {
    return Error("Empty resource name");
  }

  if (!Value::Type_IsValid(resource.type())) {
    return Error("Invalid resource type");
  }

  if (resource.type() == Value::SCALAR) {
    if (!resource.has_scalar() ||
        resource.has_ranges() ||
        resource.has_set()) {
      return Error("Invalid scalar resource");
    }

    // We do not allow negative scalar resource values or
    // non-zero values which would be represented as zero.
    if (resource.scalar().value() != 0 &&
        resource.scalar() <= Value::Scalar()) {
      return Error("Invalid scalar resource: value <= 0");
    }
  } else if (resource.type() == Value::RANGES) {
    if (resource.has_scalar() ||
        !resource.has_ranges() ||
        resource.has_set()) {
      return Error("Invalid ranges resource");
    }

    for (int i = 0; i < resource.ranges().range_size(); i++) {
      const Value::Range& range = resource.ranges().range(i);

      // Ensure the range make sense (isn't inverted).
      if (range.begin() > range.end()) {
        return Error("Invalid ranges resource: begin > end");
      }

      // Ensure ranges don't overlap (but not necessarily coalesced).
      for (int j = i + 1; j < resource.ranges().range_size(); j++) {
        if (range.begin() <= resource.ranges().range(j).begin() &&
            resource.ranges().range(j).begin() <= range.end()) {
          return Error("Invalid ranges resource: overlapping ranges");
        }
      }
    }
  } else if (resource.type() == Value::SET) {
    if (resource.has_scalar() ||
        resource.has_ranges() ||
        !resource.has_set()) {
      return Error("Invalid set resource");
    }

    for (int i = 0; i < resource.set().item_size(); i++) {
      const string& item = resource.set().item(i);

      // Ensure no duplicates.
      for (int j = i + 1; j < resource.set().item_size(); j++) {
        if (item == resource.set().item(j)) {
          return Error("Invalid set resource: duplicated elements");
        }
      }
    }
  } else {
    // Resource doesn't support TEXT or other value types.
    return Error("Unsupported resource type");
  }

  // Checks for 'disk' resource.
  if (resource.has_disk()) {
    if (resource.name() != "disk") {
      return Error(
          "DiskInfo should not be set for " + resource.name() + " resource");
    }

    const Resource::DiskInfo& disk = resource.disk();

    if (disk.has_source()) {
      const Resource::DiskInfo::Source& source = disk.source();

      switch (source.type()) {
        case Resource::DiskInfo::Source::PATH:
        case Resource::DiskInfo::Source::MOUNT:
          // `PATH` and `MOUNT` contain only `optional` members.
          break;
        case Resource::DiskInfo::Source::BLOCK:
        case Resource::DiskInfo::Source::RAW:
          if (source.has_mount()) {
            return Error(
                "Mount should not be set for " +
                Resource::DiskInfo::Source::Type_Name(source.type()) +
                " disk source");
          }

          if (source.has_path()) {
            return Error(
                "Path should not be set for " +
                Resource::DiskInfo::Source::Type_Name(source.type()) +
                " disk source");
          }

          break;
        case Resource::DiskInfo::Source::UNKNOWN:
          return Error(
              "Unsupported 'DiskInfo.Source.Type' in "
              "'" + stringify(source) + "'");
      }
    }
  }

  // Validate the reservation format.

  if (resource.reservations_size() == 0) {
    // Check for the "pre-reservation-refinement" format.

    // Check role name.
    Option<Error> error = roles::validate(resource.role());
    if (error.isSome()) {
      return error;
    }

    // Check reservation.
    if (resource.has_reservation()) {
      if (resource.reservation().has_type()) {
        return Error(
            "'Resource.ReservationInfo.type' must not be set for"
            " the 'Resource.reservation' field");
      }

      if (resource.reservation().has_role()) {
        return Error(
            "'Resource.ReservationInfo.role' must not be set for"
            " the 'Resource.reservation' field");
      }

      // Checks for the invalid state of (role, reservation) pair.
      if (resource.role() == "*") {
        return Error(
            "Invalid reservation: role \"*\" cannot be dynamically reserved");
      }
    }
  } else {
    // Check for the "post-reservation-refinement" format.

    CHECK_GT(resource.reservations_size(), 0);

    // Validate all of the roles in `reservations`.
    foreach (
        const Resource::ReservationInfo& reservation, resource.reservations()) {
      if (!reservation.has_type()) {
        return Error(
            "Invalid reservation: 'Resource.ReservationInfo.type'"
            " field must be set.");
      }

      if (!reservation.has_role()) {
        return Error(
            "Invalid reservation: 'Resource.ReservationInfo.role'"
            " field must be set.");
      }

      Option<Error> error = roles::validate(reservation.role());
      if (error.isSome()) {
        return error;
      }

      if (reservation.role() == "*") {
        return Error("Invalid reservation: role \"*\" cannot be reserved");
      }
    }

    // Check that the reservations are correctly refined.
    string ancestor = resource.reservations(0).role();
    for (int i = 1; i < resource.reservations_size(); ++i) {
      const Resource::ReservationInfo& reservation = resource.reservations(i);

      if (reservation.type() == Resource::ReservationInfo::STATIC) {
        return Error(
            "Invalid refined reservation: A refined reservation"
            " cannot be STATIC");
      }

      const string& descendant = reservation.role();

      if (!roles::isStrictSubroleOf(descendant, ancestor)) {
        return Error(
            "Invalid refined reservation: role '" + descendant + "'" +
            " is not a refinement of '" + ancestor + "'");
      }

      ancestor = descendant;
    }

    // Additionally, we allow the "pre-reservation-refinement" format to be set
    // as long as there is only one reservation, and the `Resource.role` and
    // `Resource.reservation` fields are consistent with the reservation.
    if (resource.reservations_size() == 1) {
      const Resource::ReservationInfo& reservation = resource.reservations(0);
      if (resource.has_role() && resource.role() != reservation.role()) {
        return Error(
            "Invalid resource format: 'Resource.role' field with"
            " '" + resource.role() + "' does not match the role"
            " '" + reservation.role() + "' in 'Resource.reservations'");
      }

      switch (reservation.type()) {
        case Resource::ReservationInfo::STATIC: {
          if (resource.has_reservation()) {
            return Error(
                "Invalid resource format: 'Resource.reservation' must not be"
                " set if the single reservation in 'Resource.reservations' is"
                " STATIC");
          }

          break;
        }
        case Resource::ReservationInfo::DYNAMIC: {
          if (resource.has_role() != resource.has_reservation()) {
            return Error(
                "Invalid resource format: 'Resource.role' and"
                " 'Resource.reservation' must either be both set or both not"
                " set if the single reservation in 'Resource.reservations' is"
                " DYNAMIC");
          }

          if (resource.has_reservation() &&
              resource.reservation().principal() != reservation.principal()) {
            return Error(
                "Invalid resource format: 'Resource.reservation.principal'"
                " field with '" + resource.reservation().principal() + "' does"
                " not match the principal '" + reservation.principal() + "'"
                " in 'Resource.reservations'");
          }

          if (resource.has_reservation() &&
              resource.reservation().labels() != reservation.labels()) {
            return Error(
                "Invalid resource format: 'Resource.reservation.labels' field"
                " with '" + stringify(resource.reservation().labels()) + "'"
                " does not match the labels"
                " '" + stringify(reservation.labels()) + "'"
                " in 'Resource.reservations'");
          }

          break;
        }
        case Resource::ReservationInfo::UNKNOWN: {
          return Error("Unsupported 'Resource.ReservationInfo.Type'");
        }
      }

    } else {
      CHECK_GT(resource.reservations_size(), 1);
      if (resource.has_role()) {
        return Error(
            "Invalid resource format: 'Resource.role' must not be set if"
            " there is more than one reservation in 'Resource.reservations'");
      }

      if (resource.has_reservation()) {
        return Error(
            "Invalid resource format: 'Resource.reservation' must not be set if"
            " there is more than one reservation in 'Resource.reservations'");
      }
    }
  }

  // Check that shareability is enabled for supported resource types.
  // For now, it is for persistent volumes only.
  // NOTE: We need to modify this once we extend shareability to other
  // resource types.
  if (resource.has_shared()) {
    if (resource.name() != "disk") {
      return Error("Resource " + resource.name() + " cannot be shared");
    }

    if (!resource.has_disk() || !resource.disk().has_persistence()) {
      return Error("Only persistent volumes can be shared");
    }
  }
  return None();
}


Option<Error> Resources::validate(const RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    Option<Error> error = validate(resource);
    if (error.isSome()) {
      return Error(
          "Resource '" + stringify(resource) +
          "' is invalid: " + error->message);
    }
  }

  return None();
}


bool Resources::isEmpty(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  if (resource.type() == Value::SCALAR) {
    Value::Scalar zero;
    zero.set_value(0);
    return resource.scalar() == zero;
  } else if (resource.type() == Value::RANGES) {
    return resource.ranges().range_size() == 0;
  } else if (resource.type() == Value::SET) {
    return resource.set().item_size() == 0;
  } else {
    return false;
  }
}


bool Resources::isPersistentVolume(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.has_disk() && resource.disk().has_persistence();
}


bool Resources::isDisk(
    const Resource& resource,
    const Resource::DiskInfo::Source::Type& type)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.has_disk() &&
         resource.disk().has_source() &&
         resource.disk().source().type() == type;
}


bool Resources::isReserved(
    const Resource& resource,
    const Option<string>& role)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return !isUnreserved(resource) &&
         (role.isNone() || role.get() == reservationRole(resource));
}


bool Resources::isAllocatableTo(
    const Resource& resource,
    const std::string& role)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return isUnreserved(resource) ||
         role == reservationRole(resource) ||
         roles::isStrictSubroleOf(role, reservationRole(resource));
}


bool Resources::isUnreserved(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.reservations_size() == 0;
}


bool Resources::isDynamicallyReserved(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return isReserved(resource) && (resource.reservations().rbegin()->type() ==
                                  Resource::ReservationInfo::DYNAMIC);
}


bool Resources::isRevocable(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.has_revocable();
}


bool Resources::isShared(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.has_shared();
}


bool Resources::isAllocatedToRoleSubtree(
    const Resource& resource, const string& role)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.allocation_info().role() == role ||
         roles::isStrictSubroleOf(resource.allocation_info().role(), role);
}


bool Resources::isReservedToRoleSubtree(
    const Resource& resource, const string& role)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return Resources::isReserved(resource) &&
         (Resources::reservationRole(resource) == role ||
          roles::isStrictSubroleOf(Resources::reservationRole(resource), role));
}


bool Resources::hasRefinedReservations(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.reservations_size() > 1;
}


bool Resources::hasResourceProvider(const Resource& resource)
{
  CHECK(!resource.has_role()) << resource;
  CHECK(!resource.has_reservation()) << resource;

  return resource.has_provider_id();
}


const string& Resources::reservationRole(const Resource& resource)
{
  CHECK_GT(resource.reservations_size(), 0);
  return resource.reservations().rbegin()->role();
}


bool Resources::shrink(Resource* resource, const Value::Scalar& target)
{
  if (resource->scalar() <= target) {
    return true; // Already within target.
  }

  // Some `disk` resources (e.g. MOUNT disk) are indivisible.
  // We use a containement check to verify this. Specifically,
  // if it contains a smaller version of itself, then it can
  // safely be chopped into a smaller amount.
  //
  // NOTE: If additional types of resources become indivisible,
  // this code needs updating!
  if (resource->has_disk()) {
    Resource original = *resource;

    Value::Scalar oldScalar = resource->scalar();
    *resource->mutable_scalar() = target;

    if (mesos::contains(original, *resource)) {
      return true;
    }

    // Restore the old value.
    *resource->mutable_scalar() = std::move(oldScalar);
    return false;
  }

  *resource->mutable_scalar() = target;
  return true;
}


Resources Resources::getReservationAncestor(
    const Resources& r1, const Resources& r2)
{
  CHECK(!r1.empty());
  CHECK(!r2.empty());
  CHECK(r1.toUnreserved() == r2.toUnreserved());

  Resources result = r1.toUnreserved();
  Resource ancestor = getReservationAncestor(*r1.begin(), *r2.begin());
  foreach (
      const Resource::ReservationInfo& reservation,
      ancestor.reservations()) {
    result = result.pushReservation(reservation);
  }

  return result;
}


Resource Resources::getReservationAncestor(
    const Resource& r1, const Resource& r2)
{
  Resource ancestor = r1;
  ancestor.clear_reservations();

  const int size = std::min(r1.reservations_size(), r2.reservations_size());
  for (int i = 0; i < size && r1.reservations(i) == r2.reservations(i); ++i) {
    *ancestor.add_reservations() = r1.reservations(i);
  }

  return ancestor;
}


/////////////////////////////////////////////////
// Public member functions.
/////////////////////////////////////////////////

Option<Error> Resources::Resource_::validate() const
{
  if (isShared() && sharedCount.get() < 0) {
    return Error("Invalid shared resource: count < 0");
  }

  return Resources::validate(resource);
}


bool Resources::Resource_::isEmpty() const
{
  if (isShared() && sharedCount.get() == 0) {
    return true;
  }

  return Resources::isEmpty(resource);
}


bool Resources::Resource_::contains(const Resource_& that) const
{
  // Both Resource_ objects should have the same sharedness.
  if (isShared() != that.isShared()) {
    return false;
  }

  // Assuming the wrapped Resource objects are equal, the 'contains'
  // relationship is determined by the relationship of the counters
  // for shared resources.
  if (isShared()) {
    return sharedCount.get() >= that.sharedCount.get() &&
           resource == that.resource;
  }

  // For non-shared resources just compare the protobufs.
  return mesos::contains(resource, that.resource);
}


Resources::Resource_& Resources::Resource_::operator+=(const Resource_& that)
{
  // This function assumes that the 'resource' fields are addable.

  if (!isShared()) {
    resource += that.resource;
  } else {
    // 'addable' makes sure both 'resource' fields are shared and
    // equal, so we just need to sum up the counters here.
    CHECK_SOME(sharedCount);
    CHECK_SOME(that.sharedCount);

    sharedCount = sharedCount.get() + that.sharedCount.get();
  }

  return *this;
}


Resources::Resource_& Resources::Resource_::operator-=(const Resource_& that)
{
  // This function assumes that the 'resource' fields are subtractable.

  if (!isShared()) {
    resource -= that.resource;
  } else {
    // 'subtractable' makes sure both 'resource' fields are shared and
    // equal, so we just need to subtract the counters here.
    CHECK_SOME(sharedCount);
    CHECK_SOME(that.sharedCount);

    sharedCount = sharedCount.get() - that.sharedCount.get();
  }

  return *this;
}


bool Resources::Resource_::operator==(const Resource_& that) const
{
  // Both Resource_ objects should have the same sharedness.
  if (isShared() != that.isShared()) {
    return false;
  }

  // For shared resources to be equal, the shared counts need to match.
  if (isShared() && (sharedCount.get() != that.sharedCount.get())) {
    return false;
  }

  return resource == that.resource;
}


bool Resources::Resource_::operator!=(const Resource_& that) const
{
  return !(*this == that);
}


Resources::Resources(const Resource& resource)
{
  // NOTE: Invalid and zero Resource object will be ignored.
  *this += resource;
}


Resources::Resources(Resource&& resource)
{
  // NOTE: Invalid and zero Resource object will be ignored.
  *this += std::move(resource);
}


Resources::Resources(const vector<Resource>& _resources)
{
  resourcesNoMutationWithoutExclusiveOwnership.reserve(_resources.size());
  foreach (const Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


Resources::Resources(vector<Resource>&& _resources)
{
  resourcesNoMutationWithoutExclusiveOwnership.reserve(_resources.size());
  foreach (Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += std::move(resource);
  }
}


Resources::Resources(const RepeatedPtrField<Resource>& _resources)
{
  resourcesNoMutationWithoutExclusiveOwnership.reserve(_resources.size());
  foreach (const Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


Resources::Resources(RepeatedPtrField<Resource>&& _resources)
{
  resourcesNoMutationWithoutExclusiveOwnership.reserve(_resources.size());
  foreach (Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += std::move(resource);
  }
}


bool Resources::contains(const Resources& that) const
{
  Resources remaining = *this;

  foreach (
      const Resource_Unsafe& resource_,
      that.resourcesNoMutationWithoutExclusiveOwnership) {
    // NOTE: We use _contains because Resources only contain valid
    // Resource objects, and we don't want the performance hit of the
    // validity check.
    if (!remaining._contains(*resource_)) {
      return false;
    }

    if (isPersistentVolume(resource_->resource)) {
      remaining.subtract(*resource_);
    }
  }

  return true;
}


bool Resources::contains(const Resource& that) const
{
  // NOTE: We must validate 'that' because invalid resources can lead
  // to false positives here (e.g., "cpus:-1" will return true). This
  // is because 'contains' assumes resources are valid.
  return validate(that).isNone() && _contains(Resource_(that));
}


// This function assumes all quantities with the same name are merged
// in the input `quantities` which is a guaranteed property of
// `ResourceQuantities`.
bool Resources::contains(const ResourceQuantities& quantities) const
{
  foreach (auto& quantity, quantities){
    double remaining = quantity.second.value();

    foreach (const Resource& r, get(quantity.first)) {
      switch (r.type()) {
        case Value::SCALAR: remaining -= r.scalar().value(); break;
        case Value::SET:    remaining -= r.set().item_size(); break;
        case Value::RANGES:
          foreach (const Value::Range& range, r.ranges().range()) {
            remaining -= range.end() - range.begin() + 1;
            if (remaining <= 0) {
              break;
            }
          }
          break;
        case Value::TEXT:
          LOG(FATAL) << "Unexpected TEXT type resource " << r << " in "
                     << *this;
          break;
      }

      if (remaining <= 0) {
        break;
      }
    }

    if (remaining > 0) {
      return false;
    }
  }

  return true;
}


size_t Resources::count(const Resource& that) const
{
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource == that) {
      // Return 1 for non-shared resources because non-shared
      // Resource objects in Resources are unique.
      return resource_->isShared() ? CHECK_NOTNONE(resource_->sharedCount) : 1;
    }
  }

  return 0;
}


void Resources::allocate(const string& role)
{
  foreach (
      Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    // Copy-on-write (if more than 1 reference).
    if (resource_.use_count() > 1) {
      resource_ = make_shared<Resource_>(*resource_);
    }
    resource_->resource.mutable_allocation_info()->set_role(role);
  }
}


void Resources::unallocate()
{
  foreach (
      Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource.has_allocation_info()) {
      // Copy-on-write (if more than 1 reference).
      if (resource_.use_count() > 1) {
        resource_ = make_shared<Resource_>(*resource_);
      }
      resource_->resource.clear_allocation_info();
    }
  }
}


Resources Resources::filter(
    const lambda::function<bool(const Resource&)>& predicate) const
{
  Resources result;
  result.resourcesNoMutationWithoutExclusiveOwnership.reserve(this->size());
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (predicate(resource_->resource)) {
      // We `push_back()` here instead of `add()` (which is O(n)). `add()` is
      // not necessary because we assume all Resource objects are already
      // combined in `Resources` and `filter()` should only take away
      // resource objects.
      result.resourcesNoMutationWithoutExclusiveOwnership.push_back(resource_);
    }
  }
  return result;
}


hashmap<string, Resources> Resources::reservations() const
{
  hashmap<string, Resources> result;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (isReserved(resource_->resource)) {
      result[reservationRole(resource_->resource)].add(resource_);
    }
  }

  return result;
}


Resources Resources::reserved(const Option<string>& role) const
{
  return filter(lambda::bind(isReserved, lambda::_1, role));
}


Resources Resources::allocatableTo(const string& role) const
{
  return filter(lambda::bind(isAllocatableTo, lambda::_1, role));
}


Resources Resources::allocatedToRoleSubtree(const string& role) const
{
  return filter(lambda::bind(isAllocatedToRoleSubtree, lambda::_1, role));
}


Resources Resources::reservedToRoleSubtree(const string& role) const
{
  return filter(lambda::bind(isReservedToRoleSubtree, lambda::_1, role));
}


Resources Resources::unreserved() const
{
  return filter(isUnreserved);
}


Resources Resources::persistentVolumes() const
{
  return filter(isPersistentVolume);
}


Resources Resources::revocable() const
{
  return filter(isRevocable);
}


Resources Resources::nonRevocable() const
{
  return filter(
      [](const Resource& resource) { return !isRevocable(resource); });
}


Resources Resources::shared() const
{
  return filter(isShared);
}


Resources Resources::nonShared() const
{
  return filter(
      [](const Resource& resource) { return !isShared(resource); });
}


hashmap<string, Resources> Resources::allocations() const
{
  hashmap<string, Resources> result;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    // We require that this is called only when
    // the resources are allocated.
    CHECK(resource_->resource.has_allocation_info());
    CHECK(resource_->resource.allocation_info().has_role());
    result[resource_->resource.allocation_info().role()].add(resource_);
  }

  return result;
}


// TODO(mzhu): Introduce `pushReservations`, so that for O(n) reservations
// we only need to copy `Resources` once.
Resources Resources::pushReservation(
    const Resource::ReservationInfo& reservation) const
{
  Resources result;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    Resource_ r_ = *resource_;
    r_.resource.add_reservations()->CopyFrom(reservation);
    Option<Error> validation = Resources::validate(r_.resource);
    CHECK_NONE(validation)
      << "Validation failed: " << *validation;

    result.add(std::move(r_));
  }

  return result;
}


Resources Resources::popReservation() const
{
  Resources result;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    CHECK_GT(resource_->resource.reservations_size(), 0);
    Resource_ r_ = *resource_;
    r_.resource.mutable_reservations()->RemoveLast();
    result.add(std::move(r_));
  }

  return result;
}


Resources Resources::toUnreserved() const
{
  Resources result;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (isReserved(resource_->resource)) {
      Resource_ r_ = *resource_;
      r_.resource.clear_reservations();
      result.add(std::move(r_));
    } else {
      result.add(resource_);
    }
  }

  return result;
}


Resources Resources::createStrippedScalarQuantity() const
{
  Resources stripped;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource.type() == Value::SCALAR) {
      Resource scalar;

      scalar.set_name(resource_->resource.name());
      scalar.set_type(resource_->resource.type());
      scalar.mutable_scalar()->CopyFrom(resource_->resource.scalar());

      stripped.add(std::move(scalar));
    }
  }

  return stripped;
}


Option<Resources> Resources::find(const Resources& targets) const
{
  Resources total;

  // TODO(mzhu): Traverse `Resource_` to preserve `sharedCount`.
  foreach (const Resource& target, targets) {
    Option<Resources> found = find(target);

    // Each target needs to be found!
    if (found.isNone()) {
      return None();
    }

    total += found.get();
  }

  return total;
}


Try<Resources> Resources::apply(const ResourceConversion& conversion) const
{
  return conversion.apply(*this);
}


Try<Resources> Resources::apply(const Offer::Operation& operation) const
{
  Try<vector<ResourceConversion>> conversions =
    getResourceConversions(operation);

  if (conversions.isError()) {
    return Error("Cannot get conversions: " + conversions.error());
  }

  Try<Resources> result = apply(conversions.get());
  if (result.isError()) {
    return Error(result.error());
  }

  // The following are sanity checks to ensure the amount of each type
  // of resource does not change.
  // TODO(jieyu): Currently, we only check known resource types like
  // cpus, gpus, mem, disk, ports, etc. We should generalize this.
  CHECK(result->cpus() == cpus());
  CHECK(result->gpus() == gpus());
  CHECK(result->mem() == mem());
  CHECK(result->disk() == disk());
  CHECK(result->ports() == ports());

  return result;
}


template <>
Option<Value::Scalar> Resources::get(const string& name) const
{
  Value::Scalar total;
  bool found = false;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource.name() == name &&
        resource_->resource.type() == Value::SCALAR) {
      total += resource_->resource.scalar();
      found = true;
    }
  }

  if (found) {
    return total;
  }

  return None();
}


template <>
Option<Value::Set> Resources::get(const string& name) const
{
  Value::Set total;
  bool found = false;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource.name() == name &&
        resource_->resource.type() == Value::SET) {
      total += resource_->resource.set();
      found = true;
    }
  }

  if (found) {
    return total;
  }

  return None();
}


template <>
Option<Value::Ranges> Resources::get(const string& name) const
{
  Value::Ranges total;
  bool found = false;

  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->resource.name() == name &&
        resource_->resource.type() == Value::RANGES) {
      total += resource_->resource.ranges();
      found = true;
    }
  }

  if (found) {
    return total;
  }

  return None();
}


Resources Resources::get(const string& name) const
{
  return filter([=](const Resource& resource) {
    return resource.name() == name;
  });
}


Resources Resources::scalars() const
{
  return filter([=](const Resource& resource) {
    return resource.type() == Value::SCALAR;
  });
}


set<string> Resources::names() const
{
  set<string> result;
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    result.insert(resource_->resource.name());
  }

  return result;
}


map<string, Value_Type> Resources::types() const
{
  map<string, Value_Type> result;
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    result[resource_->resource.name()] = resource_->resource.type();
  }

  return result;
}


Option<double> Resources::cpus() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("cpus");
  if (value.isSome()) {
    return value->value();
  } else {
    return None();
  }
}


Option<double> Resources::gpus() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("gpus");
  if (value.isSome()) {
    return value->value();
  } else {
    return None();
  }
}


Option<Bytes> Resources::mem() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("mem");
  if (value.isSome()) {
    return Megabytes(static_cast<uint64_t>(value->value()));
  } else {
    return None();
  }
}


Option<Bytes> Resources::disk() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("disk");
  if (value.isSome()) {
    return Megabytes(static_cast<uint64_t>(value->value()));
  } else {
    return None();
  }
}


Option<Value::Ranges> Resources::ports() const
{
  Option<Value::Ranges> value = get<Value::Ranges>("ports");
  if (value.isSome()) {
    return value.get();
  } else {
    return None();
  }
}


Option<Value::Ranges> Resources::ephemeral_ports() const
{
  Option<Value::Ranges> value = get<Value::Ranges>("ephemeral_ports");
  if (value.isSome()) {
    return value.get();
  } else {
    return None();
  }
}


Option<Resource> Resources::match(const Resource& that) const
{
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (compareResourceMetadata(resource_->resource, that)) {
      return resource_->resource;
    }
  }

  return None();
}

/////////////////////////////////////////////////
// Private member functions.
/////////////////////////////////////////////////

bool Resources::_contains(const Resource_& that) const
{
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (resource_->contains(that)) {
      return true;
    }
  }

  return false;
}


Option<Resources> Resources::find(const Resource& target) const
{
  Resources found;
  Resources total = *this;
  Resources remaining = Resources(target).toUnreserved();

  // First look in the target role, then unreserved, then any remaining role.
  vector<lambda::function<bool(const Resource&)>> predicates;

  if (isReserved(target)) {
    predicates.push_back(
        lambda::bind(isReserved, lambda::_1, reservationRole(target)));
  }

  predicates.push_back(isUnreserved);
  predicates.push_back([](const Resource&) { return true; });

  foreach (const auto& predicate, predicates) {
    foreach (
        const Resource_Unsafe& resource_,
        total.filter(predicate).resourcesNoMutationWithoutExclusiveOwnership) {
      // Need to `toUnreserved` to ignore the roles in contains().
      Resources unreserved;
      unreserved.add(resource_);
      unreserved = unreserved.toUnreserved();

      if (unreserved.contains(remaining)) {
        // The target has been found, return the result.
        // TODO(mzhu): Traverse `Resource_` to preserve `sharedCount`.
        foreach (Resource r, remaining) {
          r.mutable_reservations()->CopyFrom(
              resource_->resource.reservations());
          found.add(std::move(r));
        }

        return found;
      } else if (remaining.contains(unreserved)) {
        found.add(resource_);
        total.subtract(*resource_);
        remaining -= unreserved;
        break;
      }
    }
  }

  return None();
}


/////////////////////////////////////////////////
// Overloaded operators.
/////////////////////////////////////////////////

Resources::operator RepeatedPtrField<Resource>() const
{
  RepeatedPtrField<Resource> all;
  foreach (
      const Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    all.Add()->CopyFrom(resource_->resource);
  }

  return all;
}


bool Resources::operator==(const Resources& that) const
{
  return this->contains(that) && that.contains(*this);
}


bool Resources::operator!=(const Resources& that) const
{
  return !(*this == that);
}


Resources Resources::operator+(const Resource& that) const &
{
  Resources result = *this;
  result += that;
  return result;
}


Resources Resources::operator+(const Resource& that) &&
{
  Resources result = std::move(*this);
  result += that;
  return result;
}


Resources Resources::operator+(Resource&& that) const &
{
  Resources result = *this;
  result += std::move(that);
  return result;
}


Resources Resources::operator+(Resource&& that) &&
{
  Resources result = std::move(*this);
  result += std::move(that);
  return result;
}


Resources Resources::operator+(const Resources& that) const &
{
  Resources result = *this;
  result += that;
  return result;
}


Resources Resources::operator+(const Resources& that) &&
{
  Resources result = std::move(*this);
  result += that;
  return result;
}


Resources Resources::operator+(Resources&& that) const &
{
  Resources result = std::move(that);
  result += *this;
  return result;
}


Resources Resources::operator+(Resources&& that) &&
{
  Resources result = std::move(*this);
  result += std::move(that);
  return result;
}


void Resources::add(const Resource_& that)
{
  if (that.isEmpty()) {
    return;
  }

  bool found = false;
  foreach (
      Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (internal::addable(resource_->resource, that.resource)) {
      // Copy-on-write (if more than 1 reference).
      if (resource_.use_count() > 1) {
        resource_ = make_shared<Resource_>(*resource_);
      }

      *resource_ += that;
      found = true;
      break;
    }
  }

  // Cannot be combined with any existing Resource object.
  if (!found) {
    resourcesNoMutationWithoutExclusiveOwnership.push_back(
        make_shared<Resource_>(that));
  }
}


void Resources::add(Resource_&& that)
{
  if (that.isEmpty()) {
    return;
  }

  bool found = false;
  foreach (
      Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (internal::addable(resource_->resource, that.resource)) {
      // Copy-on-write (if more than 1 reference).
      if (resource_.use_count() > 1) {
        that += *resource_;
        resource_ = make_shared<Resource_>(std::move(that));
      } else {
        *resource_ += that;
      }

      found = true;
      break;
    }
  }

  // Cannot be combined with any existing Resource object.
  if (!found) {
    resourcesNoMutationWithoutExclusiveOwnership.push_back(
        make_shared<Resource_>(std::move(that)));
  }
}


void Resources::add(const Resource_Unsafe& that)
{
  if (that->isEmpty()) {
    return;
  }

  bool found = false;
  foreach (
      Resource_Unsafe& resource_,
      resourcesNoMutationWithoutExclusiveOwnership) {
    if (internal::addable(resource_->resource, that->resource)) {
      // Copy-on-write (if more than 1 reference).
      if (resource_.use_count() > 1) {
        resource_ = make_shared<Resource_>(*resource_);
      }

      *resource_ += *that;
      found = true;
      break;
    }
  }

  // Cannot be combined with any existing Resource object.
  if (!found) {
    resourcesNoMutationWithoutExclusiveOwnership.push_back(that);
  }
}


Resources& Resources::operator+=(const Resource_& that)
{
  if (that.validate().isNone()) {
    add(that);
  }

  return *this;
}


Resources& Resources::operator+=(Resource_&& that)
{
  if (that.validate().isNone()) {
    add(std::move(that));
  }

  return *this;
}


Resources& Resources::operator+=(const Resource& that)
{
  *this += Resource_(that);

  return *this;
}


Resources& Resources::operator+=(Resource&& that)
{
  *this += Resource_(std::move(that));

  return *this;
}


Resources& Resources::operator+=(const Resources& that)
{
  foreach (
      const Resource_Unsafe& resource_,
      that.resourcesNoMutationWithoutExclusiveOwnership) {
    add(resource_);
  }

  return *this;
}


Resources& Resources::operator+=(Resources&& that)
{
  foreach (
      const Resource_Unsafe& resource_,
      that.resourcesNoMutationWithoutExclusiveOwnership) {
    add(std::move(resource_));
  }

  return *this;
}


Resources Resources::operator-(const Resource& that) const
{
  Resources result = *this;
  result -= that;
  return result;
}


Resources Resources::operator-(const Resources& that) const
{
  Resources result = *this;
  result -= that;
  return result;
}


void Resources::subtract(const Resource_& that)
{
  if (that.isEmpty()) {
    return;
  }

  for (size_t i = 0; i < resourcesNoMutationWithoutExclusiveOwnership.size();
       i++) {
    Resource_Unsafe& resource_ =
      resourcesNoMutationWithoutExclusiveOwnership[i];

    if (internal::subtractable(resource_->resource, that)) {
      // Copy-on-write (if more than 1 reference).
      if (resource_.use_count() > 1) {
        resource_ = make_shared<Resource_>(*resource_);
      }

      *resource_ -= that;

      // Remove the resource if it has become negative or empty.
      // Note that a negative resource means the caller is
      // subtracting more than they should!
      //
      // TODO(gyliu513): Provide a stronger interface to avoid
      // silently allowing this to occur.

      // A "negative" Resource_ either has a negative sharedCount or
      // a negative scalar value.
      bool negative =
        (resource_->isShared() && resource_->sharedCount.get() < 0) ||
        (resource_->resource.type() == Value::SCALAR &&
         resource_->resource.scalar().value() < 0);

      if (negative || resource_->isEmpty()) {
        // As `resources` is not ordered, and erasing an element
        // from the middle is expensive, we swap with the last element
        // and then shrink the vector by one.
        resourcesNoMutationWithoutExclusiveOwnership[i] =
          resourcesNoMutationWithoutExclusiveOwnership.back();
        resourcesNoMutationWithoutExclusiveOwnership.pop_back();
      }

      break;
    }
  }
}


Resources& Resources::operator-=(const Resource_& that)
{
  if (that.validate().isNone()) {
    subtract(that);
  }

  return *this;
}


Resources& Resources::operator-=(const Resource& that)
{
  *this -= Resource_(that);

  return *this;
}


Resources& Resources::operator-=(const Resources& that)
{
  foreach (
      const Resource_Unsafe& resource_,
      that.resourcesNoMutationWithoutExclusiveOwnership) {
    subtract(*resource_);
  }

  return *this;
}


ostream& operator<<(ostream& stream, const Resource::DiskInfo::Source& source)
{
  const Option<string> csiSource = source.has_id() || source.has_profile()
    ? "(" + source.vendor() + "," + source.id() + "," + source.profile() + ")"
    : Option<string>::none();

  switch (source.type()) {
    case Resource::DiskInfo::Source::MOUNT:
      return stream << "MOUNT" << csiSource.getOrElse(
          source.mount().has_root() ? ":" + source.mount().root() : "");
    case Resource::DiskInfo::Source::PATH:
      return stream << "PATH" << csiSource.getOrElse(
          source.path().has_root() ? ":" + source.path().root() : "");
    case Resource::DiskInfo::Source::BLOCK:
      return stream << "BLOCK" << csiSource.getOrElse("");
    case Resource::DiskInfo::Source::RAW:
      return stream << "RAW" << csiSource.getOrElse("");
    case Resource::DiskInfo::Source::UNKNOWN:
      return stream << "UNKNOWN";
  }

  UNREACHABLE();
}


ostream& operator<<(ostream& stream, const Volume& volume)
{
  string volumeConfig = volume.container_path();

  if (volume.has_host_path()) {
    volumeConfig = volume.host_path() + ":" + volumeConfig;

    if (volume.has_mode()) {
      switch (volume.mode()) {
        case Volume::RW: volumeConfig += ":rw"; break;
        case Volume::RO: volumeConfig += ":ro"; break;
        default:
          LOG(FATAL) << "Unknown Volume mode: " << volume.mode();
          break;
      }
    }
  }

  stream << volumeConfig;

  return stream;
}


ostream& operator<<(ostream& stream, const Labels& labels)
{
  stream << "{";

  for (int i = 0; i < labels.labels().size(); i++) {
    const Label& label = labels.labels().Get(i);

    stream << label.key();

    if (label.has_value()) {
      stream << ": " << label.value();
    }

    if (i + 1 < labels.labels().size()) {
      stream << ", ";
    }
  }

  stream << "}";

  return stream;
}


ostream& operator<<(
    ostream& stream,
    const Resource::ReservationInfo& reservation)
{
  stream << Resource::ReservationInfo::Type_Name(reservation.type()) << ","
         << reservation.role();

  if (reservation.has_principal()) {
    stream << "," << reservation.principal();
  }

  if (reservation.has_labels()) {
    stream << "," << reservation.labels();
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Resource::DiskInfo& disk)
{
  if (disk.has_source()) {
    stream << disk.source();
  }

  if (disk.has_persistence()) {
    if (disk.has_source()) {
      stream << ",";
    }
    stream << disk.persistence().id();
  }

  if (disk.has_volume()) {
    stream << ":" << disk.volume();
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Resource& resource)
{
  stream << resource.name();

  if (resource.has_allocation_info()) {
    stream << "(allocated: " << resource.allocation_info().role() << ")";
  }

  if (resource.reservations_size() > 0) {
    stream << "(reservations: [";

    for (int i = 0; i < resource.reservations_size(); ++i) {
      if (i > 0) {
        stream << ",";
      }
      stream << "(" << resource.reservations(i) << ")";
    }

    stream << "])";
  }

  if (resource.has_disk()) {
    stream << "[" << resource.disk() << "]";
  }

  // Once extended revocable attributes are available, change this to a more
  // meaningful value.
  if (resource.has_revocable()) {
    stream << "{REV}";
  }

  if (resource.has_shared()) {
    stream << "<SHARED>";
  }

  stream << ":";

  switch (resource.type()) {
    case Value::SCALAR: stream << resource.scalar(); break;
    case Value::RANGES: stream << resource.ranges(); break;
    case Value::SET:    stream << resource.set();    break;
    default:
      LOG(FATAL) << "Unexpected Value type: " << resource.type();
      break;
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Resources::Resource_& resource_)
{
  stream << resource_.resource;
  if (resource_.isShared()) {
    stream << "<" << resource_.sharedCount.get() << ">";
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Resources& resources)
{
  if (resources.empty()) {
    stream << "{}";
    return stream;
  }

  Resources::const_iterator it = resources.begin();

  while (it != resources.end()) {
    stream << *it;
    if (++it != resources.end()) {
      stream << "; ";
    }
  }

  return stream;
}


// We use `JSON::protobuf` to print the resources here because these
// resources may not have been validated, or not converted to
// "post-reservation-refinement" format at this point.
ostream& operator<<(
    ostream& stream,
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  return stream << JSON::protobuf(resources);
}


bool contains(const Resource& left, const Resource& right)
{
  // NOTE: This is a necessary condition for 'contains'.
  // 'subtractable' will verify name, role, type, ReservationInfo,
  // DiskInfo, SharedInfo, RevocableInfo, and ResourceProviderID
  // compatibility.
  if (!internal::subtractable(left, right)) {
    return false;
  }

  if (left.type() == Value::SCALAR) {
    return right.scalar() <= left.scalar();
  } else if (left.type() == Value::RANGES) {
    return right.ranges() <= left.ranges();
  } else if (left.type() == Value::SET) {
    return right.set() <= left.set();
  } else {
    return false;
  }
}


Try<Resources> ResourceConversion::apply(const Resources& resources) const
{
  Resources result = resources;

  if (!result.contains(consumed)) {
    return Error(
        stringify(result) + " does not contain " +
        stringify(consumed));
  }

  result -= consumed;
  result += converted;

  if (postValidation.isSome()) {
    Try<Nothing> validation = postValidation.get()(result);
    if (validation.isError()) {
      return Error(validation.error());
    }
  }

  return result;
}

} // namespace mesos {
