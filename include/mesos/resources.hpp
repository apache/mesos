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

#include <iostream>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/values.hpp>

#include <stout/bytes.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>


// Resources come in three types: scalar, ranges, and sets. These are
// represented using protocol buffers. To make manipulation of
// resources easier within the Mesos core and for scheduler writers,
// we provide generic overloaded operators (see below) as well as a
// general Resources class that encapsulates a collection of protocol
// buffer Resource objects. The Resources class also provides a few
// static routines to allow parsing resources (e.g., from the command
// line), as well as determining whether or not a Resource object is
// valid. Note that many of these operations have not been optimized
// but instead just written for correct semantics.

namespace mesos {

// TODO(bmahler): Ensure that the underlying resources are kept
// in a flattened state: MESOS-1714.
class Resources
{
public:
  // Parses the text and returns a Resource object with the given name
  // and role. For example, "Resource r = parse("mem", "1024", "*");".
  static Try<Resource> parse(
      const std::string& name,
      const std::string& value,
      const std::string& role);

  // Parses Resources from text in the form "name:value(role);
  // name:value;...". Any name/value pair that doesn't specify a role
  // is assigned to defaultRole.
  static Try<Resources> parse(
      const std::string& text,
      const std::string& defaultRole = "*");

  // Returns true iff this resource has a name, a valid type, i.e.
  // scalar, range, or set, and has the appropriate value set for its
  // type.
  static bool isValid(const Resource& resource);

  // Returns true iff this resource is valid and allocatable. In
  // particular, a scalar is allocatable if it's value is greater than
  // zero, a ranges is allocatable if there is at least one valid
  // range in it, and a set is allocatable if it has at least one
  // item.
  static bool isAllocatable(const Resource& resource);

  // Returns true iff this resource is zero valued, i.e. is zero for
  // scalars, has a range size of zero for ranges, and has no items
  // for sets.
  static bool isZero(const Resource& resource);

  Resources() {}

  // TODO(jieyu): Consider using C++11 initializer list.
  /*implicit*/ Resources(const Resource& resource)
  {
    resources.Add()->CopyFrom(resource);
  }

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

  size_t size() const
  {
    return resources.size();
  }

  // Returns all resources in this object that are marked with the
  // specified role.
  Resources extract(const std::string& role) const;

  // Returns a Resources object with the same amount of each resource
  // type as these Resources, but with only one Resource object per
  // type and all Resource object marked as the specified role.
  Resources flatten(const std::string& role = "*") const;

  // Finds a number of resources equal to toFind in these Resources
  // and returns them marked with appropriate roles. For each resource
  // type, resources are first taken from the specified role, then
  // from '*', then from any other role.
  Option<Resources> find(
      const Resources& toFind,
      const std::string& role = "*") const;

  // Returns the Resource from these Resources that matches the
  // argument in name, type, and role, if it exists.
  Option<Resource> get(const Resource& r) const;

  // Returns all Resources from these Resources that match the
  // argument in name and type, regardless of role.
  Option<Resources> getAll(const Resource& r) const;

  template <typename T>
  T get(const std::string& name, const T& t) const;

  // Returns a Resources object with only the allocatable resources.
  Resources allocatable() const;

  // Helpers to get known resource types.
  // TODO(vinod): Fix this when we make these types as first class
  // protobufs.
  Option<double> cpus() const;
  Option<Bytes> mem() const;
  Option<Bytes> disk() const;

  // TODO(vinod): Provide a Ranges abstraction.
  Option<Value::Ranges> ports() const;

  // TODO(jieyu): Consider returning an EphemeralPorts abstraction
  // which holds the ephemeral ports allocation logic.
  Option<Value::Ranges> ephemeral_ports() const;

  typedef google::protobuf::RepeatedPtrField<Resource>::iterator
  iterator;

  typedef google::protobuf::RepeatedPtrField<Resource>::const_iterator
  const_iterator;

  iterator begin() { return resources.begin(); }
  iterator end() { return resources.end(); }

  const_iterator begin() const { return resources.begin(); }
  const_iterator end() const { return resources.end(); }

  // Using this operator makes it easy to copy a resources object into
  // a protocol buffer field.
  operator const google::protobuf::RepeatedPtrField<Resource>& () const;

  bool operator == (const Resources& that) const;
  bool operator != (const Resources& that) const;

  bool operator <= (const Resources& that) const;

  // NOTE: If any error occurs (e.g., input Resource is not valid or
  // the first operand is not a superset of the second oprand while
  // doing subtraction), the semantics is as though the second operand
  // was actually just an empty resource (as though you didn't do the
  // operation at all).
  Resources operator + (const Resource& that) const;
  Resources operator + (const Resources& that) const;
  Resources& operator += (const Resource& that);
  Resources& operator += (const Resources& that);

  Resources operator - (const Resource& that) const;
  Resources operator - (const Resources& that) const;
  Resources& operator -= (const Resource& that);
  Resources& operator -= (const Resources& that);

private:
  google::protobuf::RepeatedPtrField<Resource> resources;
};


std::ostream& operator << (std::ostream& stream, const Resource& resource);
std::ostream& operator << (std::ostream& stream, const Resources& resources);


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
