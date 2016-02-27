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

#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/values.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/strings.hpp>

using std::map;
using std::ostream;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace v1 {

/////////////////////////////////////////////////
// Helper functions.
/////////////////////////////////////////////////

bool operator==(
    const Resource::ReservationInfo& left,
    const Resource::ReservationInfo& right)
{
  return left.principal() == right.principal();
}


bool operator!=(
    const Resource::ReservationInfo& left,
    const Resource::ReservationInfo& right)
{
  return !(left == right);
}


bool operator==(const Resource::DiskInfo& left, const Resource::DiskInfo& right)
{
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

  // TODO(jieyu): Even if two Resource objects with DiskInfo have the
  // same persistence ID, they cannot be added together. In fact, this
  // shouldn't happen if we do not add resources from different
  // namespaces (e.g., across slave). Consider adding a warning.
  if (left.has_disk() && left.disk().has_persistence()) {
    return false;
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

  // NOTE: For Resource objects that have DiskInfo, we can only do
  // subtraction if they are equal.
  if (left.has_disk() && left.disk().has_persistence() && left != right) {
    return false;
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
  // DiskInfo and RevocableInfo compatibility.
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
  Resources resources;
  hashmap<string, Value_Type> nameTypes;

  foreach (const string& token, strings::tokenize(text, ";")) {
    vector<string> pair = strings::tokenize(token, ":");
    if (pair.size() != 2) {
      return Error("Bad value for resources, missing or extra ':' in " + token);
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

    if (nameTypes.contains(name) && nameTypes[name] != resource.get().type()) {
      return Error(
          "Resources with the same name ('" + name + "') but different types "
          "are not allowed");
    } else if (!nameTypes.contains(name)) {
      nameTypes[name] = resource.get().type();
    }

    resources += resource.get();
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
  if (resource.has_disk() && resource.name() != "disk") {
    return Error(
        "DiskInfo should not be set for " + resource.name() + " resource");
  }

  // Checks for the invalid state of (role, reservation) pair.
  if (resource.role() == "*" && resource.has_reservation()) {
    return Error(
        "Invalid reservation: role \"*\" cannot be dynamically reserved");
  }

  return None();
}


Option<Error> Resources::validate(
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    Option<Error> error = validate(resource);
    if (error.isSome()) {
      return Error(
          "Resource '" + stringify(resource) +
          "' is invalid: " + error.get().message);
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
    const Option<std::string>& role)
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

/////////////////////////////////////////////////
// Public member functions.
/////////////////////////////////////////////////


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


Resources::Resources(
    const google::protobuf::RepeatedPtrField<Resource>& _resources)
{
  foreach (const Resource& resource, _resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


bool Resources::contains(const Resources& that) const
{
  Resources remaining = *this;

  foreach (const Resource& resource, that.resources) {
    // NOTE: We use _contains because Resources only contain valid
    // Resource objects, and we don't want the performance hit of the
    // validity check.
    if (!remaining._contains(resource)) {
      return false;
    }

    remaining -= resource;
  }

  return true;
}


bool Resources::contains(const Resource& that) const
{
  // NOTE: We must validate 'that' because invalid resources can lead
  // to false positives here (e.g., "cpus:-1" will return true). This
  // is because 'contains' assumes resources are valid.
  return validate(that).isNone() && _contains(that);
}


Resources Resources::filter(
    const lambda::function<bool(const Resource&)>& predicate) const
{
  Resources result;
  foreach (const Resource& resource, resources) {
    if (predicate(resource)) {
      result += resource;
    }
  }
  return result;
}


hashmap<string, Resources> Resources::reserved() const
{
  hashmap<string, Resources> result;

  foreach (const Resource& resource, resources) {
    if (isReserved(resource)) {
      result[resource.role()] += resource;
    }
  }

  return result;
}


Resources Resources::reserved(const string& role) const
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


Resources Resources::flatten(
    const string& role,
    const Option<Resource::ReservationInfo>& reservation) const
{
  Resources flattened;

  foreach (Resource resource, resources) {
    resource.set_role(role);
    if (reservation.isNone()) {
      resource.clear_reservation();
    } else {
      resource.mutable_reservation()->CopyFrom(reservation.get());
    }
    flattened += resource;
  }

  return flattened;
}


// A predicate that returns true for any resource.
static bool any(const Resource&) { return true; }


Option<Resources> Resources::find(const Resource& target) const
{
  Resources found;
  Resources total = *this;
  Resources remaining = Resources(target).flatten();

  // First look in the target role, then unreserved, then any remaining role.
  // TODO(mpark): Use a lambda for 'any' instead once we get full C++11.
  vector<lambda::function<bool(const Resource&)>> predicates = {
    lambda::bind(isReserved, lambda::_1, target.role()),
    isUnreserved,
    any
  };

  foreach (const auto& predicate, predicates) {
    foreach (const Resource& resource, total.filter(predicate)) {
      // Need to flatten to ignore the roles in contains().
      Resources flattened = Resources(resource).flatten();

      if (flattened.contains(remaining)) {
        // Done!
        if (!resource.has_reservation()) {
          return found + remaining.flatten(resource.role());
        } else {
          return found +
                 remaining.flatten(resource.role(), resource.reservation());
        }
      } else if (remaining.contains(flattened)) {
        found += resource;
        total -= resource;
        remaining -= flattened;
        break;
      }
    }
  }

  return None();
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

    case Offer::Operation::RESERVE: {
      Option<Error> error = validate(operation.reserve().resources());
      if (error.isSome()) {
        return Error("Invalid RESERVE Operation: " + error.get().message);
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
        result += reserved;
      }
      break;
    }

    case Offer::Operation::UNRESERVE: {
      Option<Error> error = validate(operation.unreserve().resources());
      if (error.isSome()) {
        return Error("Invalid UNRESERVE Operation: " + error.get().message);
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

        result -= reserved;
        result += unreserved;
      }
      break;
    }

    case Offer::Operation::CREATE: {
      Option<Error> error = validate(operation.create().volumes());
      if (error.isSome()) {
        return Error("Invalid CREATE Operation: " + error.get().message);
      }

      foreach (const Resource& volume, operation.create().volumes()) {
        if (!volume.has_disk()) {
          return Error("Invalid CREATE Operation: Missing 'disk'");
        } else if (!volume.disk().has_persistence()) {
          return Error("Invalid CREATE Operation: Missing 'persistence'");
        }

        // Strip the disk info so that we can subtract it from the
        // original resources.
        // TODO(jieyu): Non-persistent volumes are not supported for
        // now. Persistent volumes can only be be created from regular
        // disk resources. Revisit this once we start to support
        // non-persistent volumes.
        Resource stripped = volume;
        stripped.clear_disk();

        if (!result.contains(stripped)) {
          return Error("Invalid CREATE Operation: Insufficient disk resources");
        }

        result -= stripped;
        result += volume;
      }
      break;
    }

    case Offer::Operation::DESTROY: {
      Option<Error> error = validate(operation.destroy().volumes());
      if (error.isSome()) {
        return Error("Invalid DESTROY Operation: " + error.get().message);
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

        Resource stripped = volume;
        stripped.clear_disk();

        result -= volume;
        result += stripped;
      }
      break;
    }

    default:
      return Error("Unknown offer operation " + stringify(operation.type()));
  }

  // This is a sanity check to ensure the amount of each type of
  // resource does not change.
  // TODO(jieyu): Currently, we only check known resource types like
  // cpus, mem, disk, ports, etc. We should generalize this.
  CHECK(result.cpus() == cpus());
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
    return value.get().value();
  } else {
    return None();
  }
}


Option<Bytes> Resources::mem() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("mem");
  if (value.isSome()) {
    return Megabytes(static_cast<uint64_t>(value.get().value()));
  } else {
    return None();
  }
}


Option<Bytes> Resources::disk() const
{
  Option<Value::Scalar> value = get<Value::Scalar>("disk");
  if (value.isSome()) {
    return Megabytes(static_cast<uint64_t>(value.get().value()));
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


bool Resources::_contains(const Resource& that) const
{
  foreach (const Resource& resource, resources) {
    if (internal::contains(resource, that)) {
      return true;
    }
  }

  return false;
}


/////////////////////////////////////////////////
// Overloaded operators.
/////////////////////////////////////////////////


Resources::operator const google::protobuf::RepeatedPtrField<Resource>&() const
{
  return resources;
}


bool Resources::operator==(const Resources& that) const
{
  return this->contains(that) && that.contains(*this);
}


bool Resources::operator!=(const Resources& that) const
{
  return !(*this == that);
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


Resources& Resources::operator+=(const Resource& that)
{
  if (validate(that).isNone() && !isEmpty(that)) {
    bool found = false;
    foreach (Resource& resource, resources) {
      if (internal::addable(resource, that)) {
        resource += that;
        found = true;
        break;
      }
    }

    // Cannot be combined with any existing Resource object.
    if (!found) {
      resources.Add()->CopyFrom(that);
    }
  }

  return *this;
}


Resources& Resources::operator+=(const Resources& that)
{
  foreach (const Resource& resource, that.resources) {
    *this += resource;
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


Resources& Resources::operator-=(const Resource& that)
{
  if (validate(that).isNone() && !isEmpty(that)) {
    for (int i = 0; i < resources.size(); i++) {
      Resource* resource = resources.Mutable(i);

      if (internal::subtractable(*resource, that)) {
        *resource -= that;

        // Remove the resource if it becomes invalid or zero. We need
        // to do the validation because we want to strip negative
        // scalar Resource object.
        if (validate(*resource).isSome() || isEmpty(*resource)) {
          resources.DeleteSubrange(i, 1);
        }

        break;
      }
    }
  }

  return *this;
}


Resources& Resources::operator-=(const Resources& that)
{
  foreach (const Resource& resource, that.resources) {
    *this -= resource;
  }

  return *this;
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
  if (disk.has_persistence()) {
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

  stream << "(" << resource.role();

  if (resource.has_reservation()) {
    stream << ", " << resource.reservation().principal();
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


ostream& operator<<(ostream& stream, const Resources& resources)
{
  Resources::const_iterator it = resources.begin();

  while (it != resources.end()) {
    stream << *it;
    if (++it != resources.end()) {
      stream << "; ";
    }
  }

  return stream;
}

} // namespace v1 {
} // namespace mesos {
