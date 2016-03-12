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

#ifndef __MESOS_V1_RESOURCES_HPP__
#define __MESOS_V1_RESOURCES_HPP__

#include <map>
#include <iosfwd>
#include <set>
#include <string>
#include <vector>

#include <google/protobuf/repeated_field.h>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/values.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
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
namespace v1 {

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
  /**
   * Returns a Resource with the given name, value, and role.
   *
   * Parses the text and returns a Resource object with the given name, value,
   * and role. For example, "Resource r = parse("mem", "1024", "*");".
   *
   * @param name The name of the Resource.
   * @param value The Resource's value.
   * @param role The role associated with the Resource.
   * @return A `Try` which contains the parsed Resource if parsing was
   *     successful, or an Error otherwise.
   */
  static Try<Resource> parse(
      const std::string& name,
      const std::string& value,
      const std::string& role);

  /**
   * Parses Resources from an input string.
   *
   * Parses Resources from text in the form of a JSON array. If that fails,
   * parses text in the form "name(role):value;name:value;...". Any resource
   * that doesn't specify a role is assigned to the provided default role. See
   * the `Resource` protobuf definition for precise JSON formatting.
   *
   * Example JSON: [{"name":cpus","type":"SCALAR","scalar":{"value":8}}]
   *
   * @param text The input string.
   * @param defaultRole The default role.
   * @return A `Try` which contains the parsed Resources if parsing was
   *     successful, or an Error otherwise.
   */
  static Try<Resources> parse(
      const std::string& text,
      const std::string& defaultRole = "*");

  /**
   * Validates a Resource object.
   *
   * Validates the given Resource object. Returns Error if it is not valid. A
   * Resource object is valid if it has a name, a valid type, i.e. scalar,
   * range, or set, has the appropriate value set, and a valid (role,
   * reservation) pair for dynamic reservation.
   *
   * @param resource The input resource to be validated.
   * @return An `Option` which contains None() if the validation was successful,
   *     or an Error if not.
   */
  static Option<Error> validate(const Resource& resource);

  /**
   * Validates the given repeated Resource protobufs.
   *
   * Validates the given repeated Resource protobufs. Returns Error if an
   * invalid Resource is found. A Resource object is valid if it has a name, a
   * valid type, i.e. scalar, range, or set, has the appropriate value set, and
   * a valid (role, reservation) pair for dynamic reservation.
   *
   * TODO(jieyu): Right now, it's the same as checking each individual Resource
   * object in the protobufs. In the future, we could add more checks that are
   * not possible if checking each Resource object individually. For example, we
   * could check multiple usage of an item in a set or a range, etc.
   *
   * @param resources The repeated Resource objects to be validated.
   * @return An `Option` which contains None() if the validation was successful,
   *     or an Error if not.
   */
  static Option<Error> validate(
      const google::protobuf::RepeatedPtrField<Resource>& resources);

  // NOTE: The following predicate functions assume that the given
  // resource is validated.
  //
  // Valid states of (role, reservation) pair in the Resource object.
  //   Unreserved         : ("*", None)
  //   Static reservation : (R, None)
  //   Dynamic reservation: (R, { principal: <framework_principal> })
  //
  // NOTE: ("*", { principal: <framework_principal> }) is invalid.

  // Tests if the given Resource object is empty.
  static bool isEmpty(const Resource& resource);

  // Tests if the given Resource object is a persistent volume.
  static bool isPersistentVolume(const Resource& resource);

  // Tests if the given Resource object is reserved. If the role is
  // specified, tests that it's reserved for the given role.
  static bool isReserved(
      const Resource& resource,
      const Option<std::string>& role = None());

  // Tests if the given Resource object is unreserved.
  static bool isUnreserved(const Resource& resource);

  // Tests if the given Resource object is dynamically reserved.
  static bool isDynamicallyReserved(const Resource& resource);

  // Tests if the given Resource object is revocable.
  static bool isRevocable(const Resource& resource);

  // Returns the summed up Resources given a hashmap<Key, Resources>.
  //
  // NOTE: While scalar resources such as "cpus" sum correctly,
  // non-scalar resources such as "ports" do not.
  //   e.g. "cpus:2" + "cpus:1" = "cpus:3"
  //        "ports:[0-100]" + "ports:[0-100]" = "ports:[0-100]"
  //
  // TODO(mpark): Deprecate this function once we introduce the
  // concept of "cluster-wide" resources which provides correct
  // semantics for summation over all types of resources. (e.g.
  // non-scalar)
  template <typename Key>
  static Resources sum(const hashmap<Key, Resources>& _resources)
  {
    Resources result;

    foreachvalue (const Resources& resources, _resources) {
      result += resources;
    }

    return result;
  }

  Resources() {}

  // TODO(jieyu): Consider using C++11 initializer list.
  /*implicit*/ Resources(const Resource& resource);

  /*implicit*/
  Resources(const std::vector<Resource>& _resources);

  /*implicit*/
  Resources(const google::protobuf::RepeatedPtrField<Resource>& _resources);

  Resources(const Resources& that) : resources(that.resources) {}

  Resources& operator=(const Resources& that)
  {
    if (this != &that) {
      resources = that.resources;
    }
    return *this;
  }

  bool empty() const { return resources.size() == 0; }

  size_t size() const { return resources.size(); }

  // Checks if this Resources is a superset of the given Resources.
  bool contains(const Resources& that) const;

  // Checks if this Resources contains the given Resource.
  bool contains(const Resource& that) const;

  // Filter resources based on the given predicate.
  Resources filter(
      const lambda::function<bool(const Resource&)>& predicate) const;

  // Returns the reserved resources, by role.
  hashmap<std::string, Resources> reservations() const;

  // Returns the reserved resources for the role, if specified.
  // Note that the "*" role represents unreserved resources,
  // and will be ignored.
  Resources reserved(const Option<std::string>& role = None()) const;

  // Returns the unreserved resources.
  Resources unreserved() const;

  // Returns the persistent volumes.
  Resources persistentVolumes() const;

  // Returns the revocable resources.
  Resources revocable() const;

  // Returns the non-revocable resources, effectively !revocable().
  Resources nonRevocable() const;

  // Returns a Resources object with the same amount of each resource
  // type as these Resources, but with all Resource objects marked as
  // the specified (role, reservation) pair. This is used to cross
  // reservation boundaries without affecting the actual resources.
  // If the optional ReservationInfo is given, the resource's
  // 'reservation' field is set. Otherwise, the resource's
  // 'reservation' field is cleared.
  Resources flatten(
      const std::string& role = "*",
      const Option<Resource::ReservationInfo>& reservation = None()) const;

  // Returns a Resources object that contains all the scalar resources
  // in this object, but with their ReservationInfo and DiskInfo
  // omitted. Note that the `role` and RevocableInfo, if any, are
  // preserved. Because we clear ReservationInfo but preserve `role`,
  // this means that stripping a dynamically reserved resource makes
  // it effectively statically reserved.
  //
  // This is intended for code that would like to aggregate together
  // Resource values without regard for metadata like whether the
  // resource is reserved or the particular volume ID in use. For
  // example, when calculating the total resources in a cluster,
  // preserving such information has a major performance cost.
  Resources createStrippedScalarQuantity() const;

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

  // Certain offer operations (e.g., RESERVE, UNRESERVE, CREATE or
  // DESTROY) alter the offered resources. The following methods
  // provide a convenient way to get the transformed resources by
  // applying the given offer operation(s). Returns an Error if the
  // offer operation(s) cannot be applied.
  Try<Resources> apply(const Offer::Operation& operation) const;

  template <typename Iterable>
  Try<Resources> apply(const Iterable& operations) const
  {
    Resources result = *this;

    foreach (const Offer::Operation& operation, operations) {
      Try<Resources> transformed = result.apply(operation);
      if (transformed.isError()) {
        return Error(transformed.error());
      }

      result = transformed.get();
    }

    return result;
  }

  // Helpers to get resource values. We consider all roles here.
  template <typename T>
  Option<T> get(const std::string& name) const;

  // Get resources of the given name.
  Resources get(const std::string& name) const;

  // Get all the resources that are scalars.
  Resources scalars() const;

  // Get the set of unique resource names.
  std::set<std::string> names() const;

  // Get the types of resources associated with each resource name.
  // NOTE: Resources of the same name must have the same type, as
  // enforced by Resources::parse().
  std::map<std::string, Value_Type> types() const;

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

  // NOTE: Non-`const` `iterator`, `begin()` and `end()` are __intentionally__
  // defined with `const` semantics in order to prevent mutable access to the
  // `Resource` objects within `resources`.
  typedef google::protobuf::RepeatedPtrField<Resource>::const_iterator
  iterator;

  typedef google::protobuf::RepeatedPtrField<Resource>::const_iterator
  const_iterator;

  const_iterator begin()
  {
    using google::protobuf::RepeatedPtrField;
    return static_cast<const RepeatedPtrField<Resource>&>(resources).begin();
  }

  const_iterator end()
  {
    using google::protobuf::RepeatedPtrField;
    return static_cast<const RepeatedPtrField<Resource>&>(resources).end();
  }

  const_iterator begin() const { return resources.begin(); }
  const_iterator end() const { return resources.end(); }

  // Using this operator makes it easy to copy a resources object into
  // a protocol buffer field.
  operator const google::protobuf::RepeatedPtrField<Resource>&() const;

  bool operator==(const Resources& that) const;
  bool operator!=(const Resources& that) const;

  // NOTE: If any error occurs (e.g., input Resource is not valid or
  // the first operand is not a superset of the second oprand while
  // doing subtraction), the semantics is as though the second operand
  // was actually just an empty resource (as though you didn't do the
  // operation at all).
  Resources operator+(const Resource& that) const;
  Resources operator+(const Resources& that) const;
  Resources& operator+=(const Resource& that);
  Resources& operator+=(const Resources& that);

  Resources operator-(const Resource& that) const;
  Resources operator-(const Resources& that) const;
  Resources& operator-=(const Resource& that);
  Resources& operator-=(const Resources& that);

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


std::ostream& operator<<(std::ostream& stream, const Resource& resource);


std::ostream& operator<<(std::ostream& stream, const Resources& resources);


std::ostream& operator<<(
    std::ostream& stream,
    const google::protobuf::RepeatedPtrField<Resource>& resources);


inline Resources operator+(
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) + right;
}


inline Resources operator-(
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) - right;
}


inline bool operator==(
    const google::protobuf::RepeatedPtrField<Resource>& left,
    const Resources& right)
{
  return Resources(left) == right;
}


template <typename Key>
hashmap<Key, Resources>& operator+=(
    hashmap<Key, Resources>& left,
    const hashmap<Key, Resources>& right)
{
  foreachpair (const Key& key, const Resources& resources, right) {
    left[key] += resources;
  }
  return left;
}


template <typename Key>
hashmap<Key, Resources> operator+(
    const hashmap<Key, Resources>& left,
    const hashmap<Key, Resources>& right)
{
  hashmap<Key, Resources> result = left;
  result += right;
  return result;
}

} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_RESOURCES_HPP__
