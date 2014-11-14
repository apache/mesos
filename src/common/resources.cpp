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

#include <vector>

#include <glog/logging.h>

#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/foreach.hpp>
#include <stout/strings.hpp>

using std::ostream;
using std::string;
using std::vector;


namespace mesos {

/////////////////////////////////////////////////
// Helper functions.
/////////////////////////////////////////////////

// Tests if we can add two Resource objects together resulting in one
// valid Resource object. For example, two Resource objects with
// different name, type or role are not addable.
static bool addable(const Resource& left, const Resource& right)
{
  return left.name() == right.name() &&
    left.type() == right.type() &&
    left.role() == right.role();
}


// Tests if we can subtract "right" from "left" resulting in one valid
// Resource object. For example, two Resource objects with different
// name, type or role are not subtractable.
// NOTE: Set substraction is always well defined, it does not require
// 'right' to be contained within 'left'. For example, assuming that
// "left = {1, 2}" and "right = {2, 3}", "left" and "right" are
// subtractable because "left - right = {1}". However, "left" does not
// contains "right".
static bool subtractable(const Resource& left, const Resource& right)
{
  return left.name() == right.name() &&
    left.type() == right.type() &&
    left.role() == right.role();
}


// Tests if "right" is contained in "left".
static bool contains(const Resource& left, const Resource& right)
{
  if (left.name() != right.name() ||
      left.type() != right.type() ||
      left.role() != right.role()) {
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


bool operator == (const Resource& left, const Resource& right)
{
  if (left.name() != right.name() ||
      left.type() != right.type() ||
      left.role() != right.role()) {
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


bool operator != (const Resource& left, const Resource& right)
{
  return !(left == right);
}


bool operator <= (const Resource& left, const Resource& right)
{
  return contains(right, left);
}


Resource& operator += (Resource& left, const Resource& right)
{
  // TODO(jieyu): Leverage += for Value to avoid copying.
  if (left.type() == Value::SCALAR) {
    left.mutable_scalar()->CopyFrom(left.scalar() + right.scalar());
  } else if (left.type() == Value::RANGES) {
    left.mutable_ranges()->CopyFrom(left.ranges() + right.ranges());
  } else if (left.type() == Value::SET) {
    left.mutable_set()->CopyFrom(left.set() + right.set());
  }

  return left;
}


Resource operator + (const Resource& left, const Resource& right)
{
  Resource result = left;
  result += right;
  return result;
}


Resource& operator -= (Resource& left, const Resource& right)
{
  // TODO(jieyu): Leverage -= for Value to avoid copying.
  if (left.type() == Value::SCALAR) {
    left.mutable_scalar()->CopyFrom(left.scalar() - right.scalar());
  } else if (left.type() == Value::RANGES) {
    left.mutable_ranges()->CopyFrom(left.ranges() - right.ranges());
  } else if (left.type() == Value::SET) {
    left.mutable_set()->CopyFrom(left.set() - right.set());
  }

  return left;
}


Resource operator - (const Resource& left, const Resource& right)
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


Try<Resources> Resources::parse(
    const string& text,
    const string& defaultRole)
{
  Resources resources;

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


bool Resources::empty(const Resource& resource)
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


/////////////////////////////////////////////////
// Public member functions.
/////////////////////////////////////////////////


Resources::Resources(const Resource& resource)
{
  // NOTE: Invalid and zero Resource object will be ignored.
  *this += resource;
}


Resources::Resources(
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  foreach (const Resource& resource, resources) {
    // NOTE: Invalid and zero Resource objects will be ignored.
    *this += resource;
  }
}


Resources Resources::extract(const string& role) const
{
  Resources r;

  foreach (const Resource& resource, resources) {
    if (resource.role() == role) {
      r += resource;
    }
  }

  return r;
}


Resources Resources::flatten(const string& role) const
{
  Resources flattened;

  foreach (Resource resource, resources) {
    resource.set_role(role);
    flattened += resource;
  }

  return flattened;
}


class RoleFilter
{
public:
  static RoleFilter any() { return RoleFilter(); }

  RoleFilter() : type(ANY) {}
  /*implicit*/ RoleFilter(const string& _role) : type(SOME), role(_role) {}

  Resources apply(const Resources& resources) const
  {
    return type == ANY? resources : resources.extract(role);
  }

private:
  enum { ANY, SOME } type;
  string role;
};


Option<Resources> Resources::find(const Resource& target) const
{
  Resources found;
  Resources total = *this;
  Resources remaining = Resources(target).flatten();

  // First look in the target role, then "*", then any remaining role.
  vector<RoleFilter> filters = {
    RoleFilter(target.role()),
    RoleFilter("*"),
    RoleFilter::any()
  };

  foreach (const RoleFilter& filter, filters) {
    foreach (const Resource& resource, filter.apply(total)) {
      // Need to flatten to ignore the roles in contains().
      Resources flattened = Resources(resource).flatten();

      if (remaining <= flattened) {
        // Done!
        return found + remaining.flatten(resource.role());
      } else if (flattened <= remaining) {
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


bool Resources::contains(const Resource& that) const
{
  foreach (const Resource& resource, resources) {
    if (mesos::contains(resource, that)) {
      return true;
    }
  }

  return false;
}


/////////////////////////////////////////////////
// Overloaded operators.
/////////////////////////////////////////////////


Resources::operator const google::protobuf::RepeatedPtrField<Resource>& () const
{
  return resources;
}


bool Resources::operator == (const Resources& that) const
{
  return *this <= that && that <= *this;
}


bool Resources::operator != (const Resources& that) const
{
  return !(*this == that);
}


bool Resources::operator <= (const Resources& that) const
{
  foreach (const Resource& resource, resources) {
    if (!that.contains(resource)) {
      return false;
    }
  }

  return true;
}


Resources Resources::operator + (const Resource& that) const
{
  Resources result = *this;
  result += that;
  return result;
}


Resources Resources::operator + (const Resources& that) const
{
  Resources result = *this;
  result += that;
  return result;
}


Resources& Resources::operator += (const Resource& that)
{
  if (validate(that).isNone() && !empty(that)) {
    bool found = false;
    foreach (Resource& resource, resources) {
      if (addable(resource, that)) {
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


Resources& Resources::operator += (const Resources& that)
{
  foreach (const Resource& resource, that.resources) {
    *this += resource;
  }

  return *this;
}


Resources Resources::operator - (const Resource& that) const
{
  Resources result = *this;
  result -= that;
  return result;
}


Resources Resources::operator - (const Resources& that) const
{
  Resources result = *this;
  result -= that;
  return result;
}


Resources& Resources::operator -= (const Resource& that)
{
  if (validate(that).isNone() && !empty(that)) {
    for (int i = 0; i < resources.size(); i++) {
      Resource* resource = resources.Mutable(i);

      if (subtractable(*resource, that)) {
        *resource -= that;

        // Remove the resource if it becomes invalid or zero. We need
        // to do the validation because we want to strip negative
        // scalar Resource object.
        if (validate(*resource).isSome() || empty(*resource)) {
          resources.DeleteSubrange(i, 1);
        }

        break;
      }
    }
  }

  return *this;
}


Resources& Resources::operator -= (const Resources& that)
{
  foreach (const Resource& resource, that.resources) {
    *this -= resource;
  }

  return *this;
}


ostream& operator << (ostream& stream, const Resource& resource)
{
  stream << resource.name() << "(" << resource.role() << "):";

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


ostream& operator << (ostream& stream, const Resources& resources)
{
  mesos::Resources::const_iterator it = resources.begin();

  while (it != resources.end()) {
    stream << *it;
    if (++it != resources.end()) {
      stream << "; ";
    }
  }

  return stream;
}

} // namespace mesos {
