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

using std::map;
using std::ostream;
using std::set;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {

/////////////////////////////////////////////////
// Helper functions.
/////////////////////////////////////////////////

bool operator==(
    const Resource::ReservationInfo& left,
    const Resource::ReservationInfo& right)
{
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
  return left.root() == right.root();
}


bool operator==(
    const Resource::DiskInfo::Source::Mount& left,
    const Resource::DiskInfo::Source::Mount& right)
{
  return left.root() == right.root();
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

  if (left.has_path() && left.path() != right.path()) {
    return false;
  }

  if (left.has_mount() && left.mount() != right.mount()) {
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

  if (left.has_persistence()) {
    return left.persistence().id() == right.persistence().id();
  }

  return true;
}


bool operator!=(const Resource::DiskInfo& left, const Resource::DiskInfo& right)
{
  return !(left == right);
}


bool operator==(const Resource& left, const Resource& right)
{
  if (left.name() != right.name() ||
      left.type() != right.type() ||
      left.role() != right.role()) {
    return false;
  }

  // Check ReservationInfo.
  if (left.has_reservation() != right.has_reservation()) {
    return false;
  }

  if (left.has_reservation() && left.reservation() != right.reservation()) {
    return false;
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

  // Check SharedInfo.
  if (left.has_shared() != right.has_shared()) {
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
  if (left.name() != right.name() ||
      left.type() != right.type() ||
      left.role() != right.role()) {
    return false;
  }

  // Check ReservationInfo.
  if (left.has_reservation() != right.has_reservation()) {
    return false;
  }

  if (left.has_reservation() && left.reservation() != right.reservation()) {
    return false;
  }

  // Check DiskInfo.
  if (left.has_disk() != right.has_disk()) { return false; }

  if (left.has_disk()) {
    if (left.disk() != right.disk()) { return false; }

    // Two non-shared resources that represent exclusive 'MOUNT' disks
    // cannot be added together; this would defeat the exclusivity.
    if (left.disk().has_source() &&
        left.disk().source().type() == Resource::DiskInfo::Source::MOUNT) {
      return false;
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
  if (left.name() != right.name() ||
      left.type() != right.type() ||
      left.role() != right.role()) {
    return false;
  }

  // Check ReservationInfo.
  if (left.has_reservation() != right.has_reservation()) {
    return false;
  }

  if (left.has_reservation() && left.reservation() != right.reservation()) {
    return false;
  }

  // Check DiskInfo.
  if (left.has_disk() != right.has_disk()) { return false; }

  if (left.has_disk()) {
    if (left.disk() != right.disk()) { return false; }

    // Two resources that represent exclusive 'MOUNT' disks cannot be
    // subtracted from each other if they are not the exact same mount;
    // this would defeat the exclusivity.
    if (left.disk().has_source() &&
        left.disk().source().type() == Resource::DiskInfo::Source::MOUNT &&
        left != right) {
      return false;
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

  return true;
}


// Tests if "right" is contained in "left".
static bool contains(const Resource& left, const Resource& right)
{
  // NOTE: This is a necessary condition for 'contains'.
  // 'subtractable' will verify name, role, type, ReservationInfo,
  // DiskInfo, SharedInfo and RevocableInfo compatibility.
  if (!subtractable(left, right)) {
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
          "Revocable resources cannot be specified at the command line; do "
          "not include a 'revocable' key in the resources JSON");
    } else if (Resources::isDynamicallyReserved(resource)) {
      return Error(
          "Dynamic reservations cannot be specified at the command line; "
          "do not include a 'reservation' key in the resources JSON");
    }

    if (nameTypes.contains(resource.name()) &&
        nameTypes[resource.name()] != resource.type()) {
      return Error(
          "Resources with the same name ('" + resource.name() + "') but "
          "different types are not allowed");
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
  resource.set_role(role);

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
  // Try to parse as a JSON Array. Otherwise, parse as a text string.
  Try<JSON::Array> json = JSON::parse<JSON::Array>(text);

  Try<vector<Resource>> resources = json.isSome() ?
    Resources::fromJSON(json.get(), defaultRole) :
    Resources::fromSimpleString(text, defaultRole);

  if (resources.isError()) {
    return Error(resources.error());
  }

  Resources result;

  // Validate individual Resource objects.
  foreach(const Resource& resource, resources.get()) {
    // If invalid, propgate error instead of skipping the resource.
    Option<Error> error = Resources::validate(resource);
    if (error.isSome()) {
      return error.get();
    }

    result.add(resource);
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
    if (!resource.has_role()) {
      resource.set_role(defaultRole);
    }

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
    size_t openParen = pair[0].find("(");
    if (openParen == string::npos) {
      name = strings::trim(pair[0]);
      role = defaultRole;
    } else {
      size_t closeParen = pair[0].find(")");
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

    // We add the Resource object even if it is empty or invalid.
    resources.push_back(resource.get());
  }

  return resources;
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

    if (resource.scalar().value() < 0) {
      return Error("Invalid scalar resource: value < 0");
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

      if (source.type() == Resource::DiskInfo::Source::PATH &&
          !source.has_path()) {
        return Error(
            "DiskInfo::Source 'type' set to 'PATH' but missing 'path' data");
      }

      if (source.type() == Resource::DiskInfo::Source::MOUNT &&
          !source.has_mount()) {
        return Error(
            "DiskInfo::Source 'type' set to 'MOUNT' but missing 'mount' data");
      }
    }
  }

  // Checks for the invalid state of (role, reservation) pair.
  if (resource.role() == "*" && resource.has_reservation()) {
    return Error(
        "Invalid reservation: role \"*\" cannot be dynamically reserved");
  }

  // Check role name.
  Option<Error> error = roles::validate(resource.role());
  if (error.isSome()) {
    return error;
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
  if (resource.type() == Value::SCALAR) {
    return resource.scalar().value() == 0;
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
  return resource.has_disk() && resource.disk().has_persistence();
}


bool Resources::isReserved(
    const Resource& resource,
    const Option<string>& role)
{
  if (role.isSome()) {
    return !isUnreserved(resource) && role.get() == resource.role();
  } else {
    return !isUnreserved(resource);
  }
}


bool Resources::isUnreserved(const Resource& resource)
{
  return resource.role() == "*" && !resource.has_reservation();
}


bool Resources::isDynamicallyReserved(const Resource& resource)
{
  return resource.has_reservation();
}


bool Resources::isRevocable(const Resource& resource)
{
  return resource.has_revocable();
}


bool Resources::isShared(const Resource& resource)
{
  return resource.has_shared();
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
  return internal::contains(resource, that.resource);
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


Resources::Resources(const vector<Resource>& _resources)
{
  foreach (const Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


Resources::Resources(const RepeatedPtrField<Resource>& _resources)
{
  foreach (const Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


bool Resources::contains(const Resources& that) const
{
  Resources remaining = *this;

  foreach (const Resource_& resource_, that.resources) {
    // NOTE: We use _contains because Resources only contain valid
    // Resource objects, and we don't want the performance hit of the
    // validity check.
    if (!remaining._contains(resource_)) {
      return false;
    }

    if (isPersistentVolume(resource_.resource)) {
      remaining.subtract(resource_);
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


size_t Resources::count(const Resource& that) const
{
  foreach (const Resource_& resource_, resources) {
    if (resource_.resource == that) {
      // Return 1 for non-shared resources because non-shared
      // Resource objects in Resources are unique.
      return resource_.isShared() ? resource_.sharedCount.get() : 1;
    }
  }

  return 0;
}


Resources Resources::filter(
    const lambda::function<bool(const Resource&)>& predicate) const
{
  Resources result;
  foreach (const Resource_& resource_, resources) {
    if (predicate(resource_.resource)) {
      result.add(resource_);
    }
  }
  return result;
}


hashmap<string, Resources> Resources::reservations() const
{
  hashmap<string, Resources> result;

  foreach (const Resource_& resource_, resources) {
    if (isReserved(resource_.resource)) {
      result[resource_.resource.role()].add(resource_);
    }
  }

  return result;
}


Resources Resources::reserved(const Option<string>& role) const
{
  return filter(lambda::bind(isReserved, lambda::_1, role));
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


Try<Resources> Resources::flatten(
    const string& role,
    const Option<Resource::ReservationInfo>& reservation) const
{
  // Check role name.
  Option<Error> error = roles::validate(role);
  if (error.isSome()) {
    return error.get();
  }

  // Checks for the invalid state of (role, reservation) pair.
  if (role == "*" && reservation.isSome()) {
    return Error(
        "Invalid reservation: role \"*\" cannot be dynamically reserved");
  }

  Resources flattened;

  foreach (Resource_ resource_, resources) {
    // With the above checks, we are certain that `resource_` will
    // remain valid after the modifications.
    resource_.resource.set_role(role);
    if (reservation.isNone()) {
      resource_.resource.clear_reservation();
    } else {
      resource_.resource.mutable_reservation()->CopyFrom(reservation.get());
    }
    flattened.add(resource_);
  }

  return flattened;
}


Resources Resources::flatten() const
{
  Try<Resources> flattened = flatten("*");
  CHECK_SOME(flattened);
  return flattened.get();
}


Resources Resources::createStrippedScalarQuantity() const
{
  Resources stripped;

  foreach (const Resource& resource, resources) {
    if (resource.type() == Value::SCALAR) {
      Resource scalar = resource;
      scalar.clear_reservation();
      scalar.clear_disk();
      scalar.clear_shared();
      stripped.add(scalar);
    }
  }

  return stripped;
}


Option<Resources> Resources::find(const Resources& targets) const
{
  Resources total;

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


Try<Resources> Resources::apply(const Offer::Operation& operation) const
{
  Resources result = *this;

  switch (operation.type()) {
    case Offer::Operation::LAUNCH:
      // Launch operation does not alter the offered resources.
      break;

    case Offer::Operation::LAUNCH_GROUP:
      // LaunchGroup operation does not alter the offered resources.
      break;

    case Offer::Operation::RESERVE: {
      Option<Error> error = validate(operation.reserve().resources());
      if (error.isSome()) {
        return Error("Invalid RESERVE Operation: " + error->message);
      }

      foreach (const Resource& reserved, operation.reserve().resources()) {
        if (!Resources::isReserved(reserved)) {
          return Error("Invalid RESERVE Operation: Resource must be reserved");
        } else if (!reserved.has_reservation()) {
          return Error("Invalid RESERVE Operation: Missing 'reservation'");
        }

        Resources unreserved = Resources(reserved).flatten();

        if (!result.contains(unreserved)) {
          return Error("Invalid RESERVE Operation: " + stringify(result) +
                       " does not contain " + stringify(unreserved));
        }

        result -= unreserved;
        result.add(reserved);
      }
      break;
    }

    case Offer::Operation::UNRESERVE: {
      Option<Error> error = validate(operation.unreserve().resources());
      if (error.isSome()) {
        return Error("Invalid UNRESERVE Operation: " + error->message);
      }

      foreach (const Resource& reserved, operation.unreserve().resources()) {
        if (!Resources::isReserved(reserved)) {
          return Error("Invalid UNRESERVE Operation: Resource is not reserved");
        } else if (!reserved.has_reservation()) {
          return Error("Invalid UNRESERVE Operation: Missing 'reservation'");
        }

        if (!result.contains(reserved)) {
          return Error("Invalid UNRESERVE Operation: " + stringify(result) +
                       " does not contain " + stringify(reserved));
        }

        Resources unreserved = Resources(reserved).flatten();

        result.subtract(reserved);
        result += unreserved;
      }
      break;
    }

    case Offer::Operation::CREATE: {
      Option<Error> error = validate(operation.create().volumes());
      if (error.isSome()) {
        return Error("Invalid CREATE Operation: " + error->message);
      }

      foreach (const Resource& volume, operation.create().volumes()) {
        if (!volume.has_disk()) {
          return Error("Invalid CREATE Operation: Missing 'disk'");
        } else if (!volume.disk().has_persistence()) {
          return Error("Invalid CREATE Operation: Missing 'persistence'");
        }

        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        // TODO(jieyu): Non-persistent volumes are not supported for
        // now. Persistent volumes can only be be created from regular
        // disk resources. Revisit this once we start to support
        // non-persistent volumes.
        Resource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, the
        // original resource must be non-shared.
        stripped.clear_shared();

        if (!result.contains(stripped)) {
          return Error("Invalid CREATE Operation: Insufficient disk resources"
                       " for persistent volume " + stringify(volume));
        }

        result.subtract(stripped);
        result.add(volume);
      }
      break;
    }

    case Offer::Operation::DESTROY: {
      Option<Error> error = validate(operation.destroy().volumes());
      if (error.isSome()) {
        return Error("Invalid DESTROY Operation: " + error->message);
      }

      foreach (const Resource& volume, operation.destroy().volumes()) {
        if (!volume.has_disk()) {
          return Error("Invalid DESTROY Operation: Missing 'disk'");
        } else if (!volume.disk().has_persistence()) {
          return Error("Invalid DESTROY Operation: Missing 'persistence'");
        }

        if (!result.contains(volume)) {
          return Error(
              "Invalid DESTROY Operation: Persistent volume does not exist");
        }

        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        Resource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, we
        // return the resource to non-shared state after destroy.
        stripped.clear_shared();

        result.subtract(volume);
        result.add(stripped);
      }
      break;
    }

    case Offer::Operation::UNKNOWN:
      return Error("Unknown offer operation");
  }

  // The following are sanity checks to ensure the amount of each type of
  // resource does not change.
  // TODO(jieyu): Currently, we only check known resource types like
  // cpus, gpus, mem, disk, ports, etc. We should generalize this.

  CHECK(result.cpus() == cpus());
  CHECK(result.gpus() == gpus());
  CHECK(result.mem() == mem());
  CHECK(result.disk() == disk());
  CHECK(result.ports() == ports());

  return result;
}


template <>
Option<Value::Scalar> Resources::get(const string& name) const
{
  Value::Scalar total;
  bool found = false;

  foreach (const Resource& resource, resources) {
    if (resource.name() == name &&
        resource.type() == Value::SCALAR) {
      total += resource.scalar();
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

  foreach (const Resource& resource, resources) {
    if (resource.name() == name &&
        resource.type() == Value::SET) {
      total += resource.set();
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

  foreach (const Resource& resource, resources) {
    if (resource.name() == name &&
        resource.type() == Value::RANGES) {
      total += resource.ranges();
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
  foreach (const Resource& resource, resources) {
    result.insert(resource.name());
  }

  return result;
}


map<string, Value_Type> Resources::types() const
{
  map<string, Value_Type> result;
  foreach (const Resource& resource, resources) {
    result[resource.name()] = resource.type();
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


/////////////////////////////////////////////////
// Private member functions.
/////////////////////////////////////////////////

bool Resources::_contains(const Resource_& that) const
{
  foreach (const Resource_& resource_, resources) {
    if (resource_.contains(that)) {
      return true;
    }
  }

  return false;
}


Option<Resources> Resources::find(const Resource& target) const
{
  Resources found;
  Resources total = *this;
  Resources remaining = Resources(target).flatten();

  // First look in the target role, then unreserved, then any remaining role.
  vector<lambda::function<bool(const Resource&)>> predicates = {
    lambda::bind(isReserved, lambda::_1, target.role()),
    isUnreserved,
    [](const Resource&) { return true; }
  };

  foreach (const auto& predicate, predicates) {
    foreach (const Resource_& resource_, total.filter(predicate)) {
      // Need to flatten to ignore the roles in contains().
      Resources flattened = Resources(resource_.resource).flatten();

      if (flattened.contains(remaining)) {
        // The target has been found, return the result.
        if (!resource_.resource.has_reservation()) {
          Try<Resources> _flattened =
            remaining.flatten(resource_.resource.role());

          CHECK_SOME(_flattened);
          return found + _flattened.get();
        } else {
          Try<Resources> _flattened = remaining.flatten(
              resource_.resource.role(), resource_.resource.reservation());

          CHECK_SOME(_flattened);
          return found + _flattened.get();
        }
      } else if (remaining.contains(flattened)) {
        found.add(resource_);
        total.subtract(resource_);
        remaining -= flattened;
        break;
      }
    }
  }

  return None();
}


/////////////////////////////////////////////////
// Overloaded operators.
/////////////////////////////////////////////////

Resources::operator const RepeatedPtrField<Resource>() const
{
  RepeatedPtrField<Resource> all;
  foreach(const Resource& resource, resources) {
    all.Add()->CopyFrom(resource);
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


Resources Resources::operator+(const Resource_& that) const
{
  Resources result = *this;
  result += that;
  return result;
}


Resources Resources::operator+(const Resource& that) const
{
  Resources result = *this;
  result += that;
  return result;
}


Resources Resources::operator+(const Resources& that) const
{
  Resources result = *this;
  result += that;
  return result;
}


void Resources::add(const Resource_& that)
{
  if (that.isEmpty()) {
    return;
  }

  bool found = false;
  foreach (Resource_& resource_, resources) {
    if (internal::addable(resource_.resource, that)) {
      resource_ += that;
      found = true;
      break;
    }
  }

  // Cannot be combined with any existing Resource object.
  if (!found) {
    resources.push_back(that);
  }
}


Resources& Resources::operator+=(const Resource_& that)
{
  if (that.validate().isNone()) {
    add(that);
  }

  return *this;
}


Resources& Resources::operator+=(const Resource& that)
{
  *this += Resource_(that);

  return *this;
}


Resources& Resources::operator+=(const Resources& that)
{
  foreach (const Resource_& resource_, that) {
    add(resource_);
  }

  return *this;
}


Resources Resources::operator-(const Resource_& that) const
{
  Resources result = *this;
  result -= that;
  return result;
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

  for (size_t i = 0; i < resources.size(); i++) {
    Resource_& resource_ = resources[i];

    if (internal::subtractable(resource_.resource, that)) {
      resource_ -= that;

      // Remove the resource if it has become negative or empty.
      // Note that a negative resource means the caller is
      // subtracting more than they should!
      //
      // TODO(gyliu513): Provide a stronger interface to avoid
      // silently allowing this to occur.

      // A "negative" Resource_ either has a negative sharedCount or
      // a negative scalar value.
      bool negative =
        (resource_.isShared() && resource_.sharedCount.get() < 0) ||
        (resource_.resource.type() == Value::SCALAR &&
         resource_.resource.scalar().value() < 0);

      if (negative || resource_.isEmpty()) {
        // As `resources` is not ordered, and erasing an element
        // from the middle is expensive, we swap with the last element
        // and then shrink the vector by one.
        resources[i] = resources.back();
        resources.pop_back();
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
  foreach (const Resource_& resource_, that) {
    subtract(resource_);
  }

  return *this;
}


ostream& operator<<(ostream& stream, const Resource::DiskInfo::Source& source)
{
  switch (source.type()) {
    case Resource::DiskInfo::Source::MOUNT:
      return stream << "MOUNT:" + source.mount().root();
    case Resource::DiskInfo::Source::PATH:
      return stream << "PATH:" + source.path().root();
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


ostream& operator<<(ostream& stream, const Resource& resource)
{
  stream << resource.name();

  stream << "(" << resource.role();

  if (resource.has_reservation()) {
    const Resource::ReservationInfo& reservation = resource.reservation();

    if (reservation.has_principal()) {
      stream << ", " << reservation.principal();
    }

    if (reservation.has_labels()) {
      stream << ", " << reservation.labels();
    }
  }

  stream << ")";

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


ostream& operator<<(
    ostream& stream,
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  return stream << Resources(resources);
}

} // namespace mesos {
