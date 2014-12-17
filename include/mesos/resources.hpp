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
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/values.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
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

// NOTE: Resource objects stored in the class are always valid and
// kept combined if possible. It is the caller's responsibility to
// validate any Resource object or repeated Resource protobufs before
// constructing a Resources object. Otherwise, invalid Resource
// objects will be silently stripped. Invalid Resource objects will
// also be silently ignored when used in arithmetic operations (e.g.,
// +=, -=, etc.).
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

  // Validates the given Resource object. Returns Error if it is not
  // valid. A Resource object is valid if it has a name, a valid type,
  // i.e. scalar, range, or set, and has the appropriate value set.
  static Option<Error> validate(const Resource& resource);

  // Validates the given protobufs.
  // TODO(jieyu): Right now, it's the same as checking each individual
  // Resource object in the protobufs. In the future, we could add
  // more checks that are not possible if checking each Resource
  // object individually. For example, we could check multiple usage
  // of an item in a set or a ranges, etc.
  static Option<Error> validate(
      const google::protobuf::RepeatedPtrField<Resource>& resources);

  // Tests if the given Resource object is empty.
  static bool empty(const Resource& resource);

  Resources() {}

  // TODO(jieyu): Consider using C++11 initializer list.
  /*implicit*/ Resources(const Resource& resource);

  /*implicit*/
  Resources(const google::protobuf::RepeatedPtrField<Resource>& _resources);

  Resources(const Resources& that) : resources(that.resources) {}

  Resources& operator = (const Resources& that)
  {
    if (this != &that) {
      resources = that.resources;
    }
    return *this;
  }

  bool empty() const { return resources.size() == 0; }

  // Checks if this Resources is a superset of the given Resources.
  bool contains(const Resources& that) const;

  // Checks if this Resources contains the given Resource.
  bool contains(const Resource& that) const;

  // Returns the reserved resources, by role.
  hashmap<std::string, Resources> reserved() const;

  // Returns the reserved resources for the role. Note that the "*"
  // role represents unreserved resources, and will be ignored.
  Resources reserved(const std::string& role) const;

  // Returns the unreserved resources.
  Resources unreserved() const;

  // Returns a Resources object with the same amount of each resource
  // type as these Resources, but with all Resource objects marked as
  // the specified role.
  Resources flatten(const std::string& role = "*") const;

  // Finds a Resources object with the same amount of each resource
  // type as "targets" from these Resources. The roles specified in
  // "targets" set the preference order. For each resource type,
  // resources are first taken from the specified role, then from '*',
  // then from any other role.
  // TODO(jieyu): 'find' contains some allocation logic for scalars and
  // fixed set / range elements. However, this is not sufficient for
  // schedulers that want, say, any N available ports. We should
  // consider moving this to an internal "allocation" library for our
  // example frameworks to leverage.
  Option<Resources> find(const Resources& targets) const;

  // Helpers to get resource values. We consider all roles here.
  template <typename T>
  Option<T> get(const std::string& name) const;

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

  // This is an abstraction for describing a transformation that can
  // be applied to Resources. Transformations cannot not alter the
  // quantity, or the static role of the resources.
  class Transformation
  {
  public:
    virtual ~Transformation() {}

    // Returns the result of the transformation, applied to the given
    // 'resources'. Returns an Error if the transformation cannot be
    // applied, or the transformation invariants do not hold.
    Try<Resources> operator () (const Resources& resources) const;

  protected:
    virtual Try<Resources> apply(const Resources& resources) const = 0;
  };

  // Represents a sequence of transformations, the transformations are
  // applied in an all-or-nothing manner. We follow the composite
  // pattern here.
  class CompositeTransformation : public Transformation
  {
  public:
    CompositeTransformation() {}

    ~CompositeTransformation()
    {
      foreach (Transformation* transformation, transformations) {
        delete transformation;
      }
    }

    // TODO(jieyu): Consider using unique_ptr here once we finialize
    // our style guide for unique_ptr.
    template <typename T>
    void add(const T& t)
    {
      transformations.push_back(new T(t));
    }

  protected:
    virtual Try<Resources> apply(const Resources& resources) const;

  private:
    std::vector<Transformation*> transformations;
  };

  // Acquires a persistent disk from a regular disk resource.
  class AcquirePersistentDisk : public Transformation
  {
  public:
    AcquirePersistentDisk(const Resource& _disk);

  protected:
    virtual Try<Resources> apply(const Resources& resources) const;

  private:
    // The target persistent disk resource to acquire.
    Resource disk;
  };

private:
  // Similar to 'contains(const Resource&)' but skips the validity
  // check. This can be used to avoid the performance overhead of
  // calling 'contains(const Resource&)' when the resource can be
  // assumed valid (e.g. it's inside a Resources).
  //
  // TODO(jieyu): Measure performance overhead of validity check to
  // ensure this is warranted.
  bool _contains(const Resource& that) const;

  // Similar to the public 'find', but only for a single Resource
  // object. The target resource may span multiple roles, so this
  // returns Resources.
  Option<Resources> find(const Resource& target) const;

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
