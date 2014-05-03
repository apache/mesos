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

#ifndef __RESOURCES_HPP__
#define __RESOURCES_HPP__

#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/values.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>


/**
 * Resources come in three types: scalar, ranges, and sets. These are
 * represented using protocol buffers. To make manipulation of
 * resources easier within the Mesos core and for scheduler writers,
 * we provide generic overloaded opertors (see below) as well as a
 * general Resources class that encapsulates a collection of protocol
 * buffer Resource objects. The Resources class also provides a few
 * static routines to allow parsing resources (e.g., from the command
 * line), as well as determining whether or not a Resource object is
 * valid or allocatable. Note that many of these operations have not
 * been optimized but instead just written for correct semantics.
 *
 * Note! A resource is described by a tuple (name, type, role). Doing
 * "arithmetic" operations (those defined below) on two resources of
 * the same name but different type, or the same name and type but
 * different roles, doesn't make sense, so it's semantics are as
 * though the second operand was actually just an empty resource
 * (as though you didn't do the operation at all). In addition,
 * doing operations on two resources of the same type but different
 * names is a no-op.
 */


namespace mesos {


bool operator == (const Resource& left, const Resource& right);
bool operator != (const Resource& left, const Resource& right);
bool operator <= (const Resource& left, const Resource& right);
Resource operator + (const Resource& left, const Resource& right);
Resource operator - (const Resource& left, const Resource& right);
Resource& operator += (Resource& left, const Resource& right);
Resource& operator -= (Resource& left, const Resource& right);
// Return true iff both Resources have the same name, type, and role.
bool matches(const Resource& left, const Resource& right);

std::ostream& operator << (std::ostream& stream, const Resource& resource);


class Resources
{
public:
  Resources() {}

  /*implicit*/
  Resources(const google::protobuf::RepeatedPtrField<Resource>& _resources)
  {
    resources.MergeFrom(_resources);
  }

  Resources(const Resources& that)
  {
    resources.MergeFrom(that.resources);
  }

  Resources& operator = (const Resources& that)
  {
    if (this != &that) {
      resources.Clear();
      resources.MergeFrom(that.resources);
    }

    return *this;
  }

  /**
   * Returns a Resources object with only the allocatable resources.
   */
  Resources allocatable() const
  {
    Resources result;

    foreach (const Resource& resource, resources) {
      if (isAllocatable(resource)) {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    return result;
  }

  size_t size() const
  {
    return resources.size();
  }

  /**
   * Using this operator makes it easy to copy a resources object into
   * a protocol buffer field.
   */
  operator const google::protobuf::RepeatedPtrField<Resource>& () const
  {
    return resources;
  }

  bool operator == (const Resources& that) const
  {
    if (size() != that.size()) {
      return false;
    }

    foreach (const Resource& resource, resources) {
      Option<Resource> option = that.get(resource);
      if (option.isNone()) {
        return false;
      } else {
        if (!(resource == option.get())) {
          return false;
        }
      }
    }

    return true;
  }

  bool operator != (const Resources& that) const
  {
    return !(*this == that);
  }

  bool operator <= (const Resources& that) const
  {
    foreach (const Resource& resource, resources) {
      Option<Resource> option = that.get(resource);
      if (option.isNone()) {
        return false;
      } else {
        if (!(resource <= option.get())) {
          return false;
        }
      }
    }

    return true;
  }

  Resources operator + (const Resources& that) const
  {
    Resources result(*this);

    foreach (const Resource& resource, that.resources) {
      result += resource;
    }

    return result;
  }

  Resources operator - (const Resources& that) const
  {
    Resources result(*this);

    foreach (const Resource& resource, that.resources) {
      result -= resource;
    }

    return result;
  }

  Resources& operator += (const Resources& that)
  {
    foreach (const Resource& resource, that.resources) {
      *this += resource;
    }

    return *this;
  }

  Resources& operator -= (const Resources& that)
  {
    foreach (const Resource& resource, that.resources) {
      *this -= resource;
    }

    return *this;
  }

  Resources operator + (const Resource& that) const
  {
    Resources result;

    bool added = false;

    foreach (const Resource& resource, resources) {
      if (matches(resource, that)) {
        result.resources.Add()->MergeFrom(resource + that);
        added = true;
      } else {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    if (!added) {
      result.resources.Add()->MergeFrom(that);
    }

    return result;
  }

  Resources operator - (const Resource& that) const
  {
    Resources result;

    foreach (const Resource& resource, resources) {
      if (matches(resource, that)) {
        Resource r = resource - that;
        if (!isZero(r)) {
          result.resources.Add()->MergeFrom(r);
        }
      } else {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    return result;
  }

  Resources& operator += (const Resource& that)
  {
    *this = *this + that;
    return *this;
  }

  Resources& operator -= (const Resource& that)
  {
    *this = *this - that;
    return *this;
  }

  /**
   * Returns a Resources object with the same amount of each resource
   * type as these Resources, but with only one Resource object per
   * type and all Resource object marked as the specified role.
   */
  Resources flatten(const std::string& role = "*") const;

  /**
   * Returns all resources in this object that are marked with the
   * specified role.
   */
  Resources extract(const std::string& role) const;

  /**
   * Finds a number of resources equal to toFind in these Resources
   * and returns them marked with appropriate roles. For each resource
   * type, resources are first taken from the specified role, then
   * from '*', then from any other role.
   */
  Option<Resources> find(
      const Resources& toFind,
      const std::string& role = "*") const;

  /**
   * Returns the Resource from these Resources that matches the argument
   * in name, type, and role, if it exists.
   */
  Option<Resource> get(const Resource& r) const;

  /**
   * Returns all Resources from these Resources that match the argument
   * in name and type, regardless of role.
   */
  Option<Resources> getAll(const Resource& r) const;

  template <typename T>
  T get(const std::string& name, const T& t) const;

  // Helpers to get known resource types.
  // TODO(vinod): Fix this when we make these types as first class protobufs.
  Option<double> cpus() const;
  Option<Bytes> mem() const;
  Option<Bytes> disk() const;
  // TODO(vinod): Provide a Ranges abstraction.
  Option<Value::Ranges> ports() const;

  typedef google::protobuf::RepeatedPtrField<Resource>::iterator
  iterator;

  typedef google::protobuf::RepeatedPtrField<Resource>::const_iterator
  const_iterator;

  iterator begin() { return resources.begin(); }
  iterator end() { return resources.end(); }

  const_iterator begin() const { return resources.begin(); }
  const_iterator end() const { return resources.end(); }

  /**
   * Parses the value and returns a Resource with the given name and role.
   */
  static Try<Resource> parse(
      const std::string& name,
      const std::string& value,
      const std::string& role);

  /**
   * Parses resources in the form "name:value (role);name:value...".
   * Any name/value pair that doesn't specify a role is assigned to defaultRole.
   */
  static Try<Resources> parse(
      const std::string& s,
      const std::string& defaultRole = "*");

  /**
   * Returns true iff this resource has a name, a valid type, i.e. scalar,
   * range, or set, and has the appropriate value set for its type.
   */
  static bool isValid(const Resource& resource);

  /**
   * Returns true iff this resource is valid and allocatable. In particular,
   * a scalar is allocatable if it's value is greater than zero, a ranges
   * is allocatable if there is at least one valid range in it, and a set
   * is allocatable if it has at least one item.
   */
  static bool isAllocatable(const Resource& resource);

  /**
   * Returns true iff this resource is zero valued, i.e. is zero for scalars,
   * has a range size of zero for ranges, and has no items for sets.
   */
  static bool isZero(const Resource& resource);

private:
  google::protobuf::RepeatedPtrField<Resource> resources;
};


template <>
inline Value::Scalar Resources::get(
    const std::string& name,
    const Value::Scalar& scalar) const
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

  return scalar;
}


template <>
inline Value::Ranges Resources::get(
    const std::string& name,
    const Value::Ranges& ranges) const
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

  return ranges;
}


template <>
inline Value::Set Resources::get(
    const std::string& name,
    const Value::Set& set) const
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

  return set;
}


inline std::ostream& operator << (
    std::ostream& stream,
    const Resources& resources)
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


inline std::ostream& operator << (
    std::ostream& stream,
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  return stream << Resources(resources);
}


inline Resources operator + (
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) + right;
}


inline Resources operator - (
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) - right;
}


inline bool operator == (
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) == right;
}

} // namespace mesos {

#endif // __RESOURCES_HPP__
