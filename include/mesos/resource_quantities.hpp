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

#ifndef __COMMON_RESOURCE_QUANTITIES_HPP__
#define __COMMON_RESOURCE_QUANTITIES_HPP__

#include <string>
#include <utility>

#include <boost/container/small_vector.hpp>

#include <mesos/mesos.hpp>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

namespace mesos {

// Forward declaration.
class Resources;

class ResourceLimits;


// An efficient collection of resource quantities. All values are guaranteed
// to be positive and finite.
//
// E.g. [("cpus", 4.5), ("gpus", 0.1), ("ports", 1000)]
//
// Absent resource entries imply there is no (zero) such resources.
//
// Notes on handling negativity and arithmetic operations: values are guaranteed
// to be positive. This is achieved by construction validation and no public
// mutation interfaces. During construction, `Value::Scalar` arguments must be
// non-negative and finite, and entries with zero quantities will be dropped
// silently. Invalid arguments will result in an error where `Try`
// is returned. For arithmetic operations, non-positive values are silently
// dropped--this is consist with `class Resources`.
//
// Note for posterity, the status quo prior to this class
// was to use stripped-down `Resources` objects for storing
// quantities, however this approach:
//
//   (1) did not support quantities of non-scalar resources;
//   (2) was error prone, the caller must ensure that no
//       `Resource` metatdata (e.g. `DiskInfo`) is present;
//   (3) had poor performance, given the `Resources` storage
//       model and arithmetic implementation have to operate
//       on broader invariants.
class ResourceQuantities
{
public:
  // Parse an input string of semicolon separated "name:number" pairs.
  // Duplicate names are allowed in the input and will be merged into one entry.
  // Entries with zero values will be silently dropped.
  //
  // Example: "cpus:10;mem:1024;ports:3"
  //          "cpus:10; mem:1024; ports:3"
  // NOTE: we will trim the whitespace around the pair and in the number.
  // However, whitespace in "c p us:10" are preserved and will be parsed to
  // {"c p us", 10}. This is consistent with `Resources::fromSimpleString()`.
  //
  // Numbers must be non-negative and finite, otherwise an `Error`
  // will be returned.
  static Try<ResourceQuantities> fromString(const std::string& text);

  // Take scalar `Resources` and combine them into `ResourceQuantities`.
  // Only the resource name and its scalar value are used and the rest of the
  // meta-data is ignored. It is caller's responsibility to ensure all
  // `Resource` entries are of scalar type. Otherwise a `CHECK` error will
  // be triggered.
  static ResourceQuantities fromScalarResources(const Resources& resources);

  // Same as above, but takes a single Resource that must be valid
  // and of scalar type.
  static ResourceQuantities fromScalarResource(const Resource& resource);

  // Take `Resources` and combine them into `ResourceQuantities`. This function
  // assumes that the provided resources have already been validated; for
  // example, it assumes that ranges do not overlap and that sets do not contain
  // duplicate items.
  static ResourceQuantities fromResources(const Resources& resources);

  // Returns the summed up `ResourceQuantities` given a
  // `hashmap<Key, ResourceQuantities>`.
  template <typename Key>
  static ResourceQuantities sum(const hashmap<Key, ResourceQuantities>& map)
  {
    ResourceQuantities result;

    foreachvalue (const ResourceQuantities& quantities, map) {
      result += quantities;
    }

    return result;
  }

  ResourceQuantities();

  explicit ResourceQuantities(
    const google::protobuf::Map<std::string, Value::Scalar>& map);

  ResourceQuantities(const ResourceQuantities& that) = default;
  ResourceQuantities(ResourceQuantities&& that) = default;

  ResourceQuantities& operator=(const ResourceQuantities& that) = default;
  ResourceQuantities& operator=(ResourceQuantities&& that) = default;

  typedef boost::container::small_vector_base<
      std::pair<std::string, Value::Scalar> >::const_iterator iterator;
  typedef boost::container::small_vector_base<
      std::pair<std::string, Value::Scalar> >::const_iterator const_iterator;

  // NOTE: Non-`const` `iterator`, `begin()` and `end()` are __intentionally__
  // defined with `const` semantics in order to prevent mutation during
  // iteration. Mutation is allowed with the `[]` operator.
  const_iterator begin();
  const_iterator end();

  const_iterator begin() const { return quantities.begin(); }
  const_iterator end() const { return quantities.end(); }

  size_t size() const { return quantities.size(); };

  bool empty() const { return quantities.empty(); }

  // Returns the quantity scalar value if a quantity with the given name.
  // If the given name is absent, return zero.
  Value::Scalar get(const std::string& name) const;

  bool contains(const ResourceQuantities& quantities) const;

  bool operator==(const ResourceQuantities& quantities) const;
  bool operator!=(const ResourceQuantities& quantities) const;

  ResourceQuantities& operator+=(const ResourceQuantities& quantities);
  ResourceQuantities& operator-=(const ResourceQuantities& quantities);

  ResourceQuantities operator+(const ResourceQuantities& quantities) const;
  ResourceQuantities operator-(const ResourceQuantities& quantities) const;

private:
  friend class ResourceLimits;

  void add(const std::string& name, const Value::Scalar& scalar);
  void add(const std::string& name, double value);

  // List of name quantity pairs sorted by name.
  // Arithmetic and comparison operations benefit from this sorting.
  //
  // Pre-allocate space for first-class resources, plus some margins.
  // This needs to be updated as introduce more first-class resources.
  // [cpus, disk, gpus, mem, ports]
  boost::container::small_vector<std::pair<std::string, Value::Scalar>, 7>
    quantities;
};


std::ostream& operator<<(std::ostream& stream, const ResourceQuantities& q);


// An efficient collection of resource limits. All values are guaranteed
// to be non-negative and finite.
//
// E.g. [("cpus", 4.5), ("gpus", 0.1), ("ports", 1000)]
//
// Difference with `class ResourceQuantities`:
// The main difference resides in the semantics of absent entries and, in turn,
// whether zero values are preserved.
//
//   (1) Absent entry in `ResourceQuantities` implies zero quantity. While in
//   `ResourceLimits`, absence implies no limit (i.e. infinite amount). This
//   affects the semantics of the contains operation.
//
//   (2) Zero value preservation: `ResourceLimits` keeps zero value entries
//   while `ResourceQuantities` silently drops them. This is in accordance with
//   the absence semantic. Zero value and absence are equivalent in
//   `ResourceQuantities`. While in `ResourceLimits`, they are not.
class ResourceLimits
{
public:
  // Parse an input string of semicolon separated "name:number" pairs.
  // Entries with zero values will be preserved (unlike `ResourceQuantities`).
  // Duplicate names are NOT allowed in the input, otherwise an `Error` will
  // be returned.
  //
  // Example: "cpus:10;mem:1024;ports:0"
  //          "cpus:10; mem:1024; ports:0"
  // NOTE: we will trim the whitespace around the pair and in the number.
  // However, whitespace in "c p us:10" are preserved and will be parsed to
  // {"c p us", 10}. This is consistent with `Resources::fromSimpleString()`.
  //
  // Numbers must be non-negative and finite, otherwise an `Error`
  // will be returned.
  static Try<ResourceLimits> fromString(const std::string& text);

  ResourceLimits();

  explicit ResourceLimits(
    const google::protobuf::Map<std::string, Value::Scalar>& map);

  ResourceLimits(const ResourceLimits& that) = default;
  ResourceLimits(ResourceLimits&& that) = default;

  ResourceLimits& operator=(const ResourceLimits& that) = default;
  ResourceLimits& operator=(ResourceLimits&& that) = default;

  typedef boost::container::small_vector_base<
      std::pair<std::string, Value::Scalar> >::const_iterator iterator;
  typedef boost::container::small_vector_base<
      std::pair<std::string, Value::Scalar> >::const_iterator const_iterator;

  // NOTE: Non-`const` `iterator`, `begin()` and `end()` are __intentionally__
  // defined with `const` semantics in order to prevent mutation during
  // iteration. Mutation is allowed with the `[]` operator.
  const_iterator begin();
  const_iterator end();

  const_iterator begin() const { return limits.begin(); }
  const_iterator end() const { return limits.end(); }

  size_t size() const { return limits.size(); };

  bool empty() const { return limits.empty(); }

  // Returns the limit of the resource with the given name.
  // If there is no explicit limit for the resource, return `None()`.
  // Note, `None()` implies that the limit of the resource is infinite.
  Option<Value::Scalar> get(const std::string& name) const;

  // Due to the absence-means-infinite semantic of limits, absent entries,
  // intuitively, are considered to contain entries with finite scalar values.
  // For example:
  //    `[ ]`` will always contain any other `ResourceLimits`.
  //    `[("cpu":1)]` contains `[("mem":1)]` and vice versa.
  //    `[("cpu":1)]` will not contain `[("cpu":2)]`.
  bool contains(const ResourceLimits& right) const;

  bool operator==(const ResourceLimits& limits) const;
  bool operator!=(const ResourceLimits& limits) const;

  bool contains(const ResourceQuantities& quantities) const;

  ResourceLimits& operator-=(const ResourceQuantities& quantities);
  ResourceLimits operator-(const ResourceQuantities& quantities) const;

private:
  // Set the limit of the resource with `name` to `scalar`.
  // Note, the existing limit of the resource will be overwritten.
  void set(const std::string& name, const Value::Scalar& scalar);

  // List of name limit pairs sorted by name.
  // Arithmetic and comparison operations benefit from this sorting.
  //
  // Pre-allocate space for first-class resources, plus some margins.
  // This needs to be updated as introduce more first-class resources.
  // [cpus, disk, gpus, mem, ports]
  boost::container::small_vector<std::pair<std::string, Value::Scalar>, 7>
    limits;
};


std::ostream& operator<<(std::ostream& stream, const ResourceLimits& limits);


} // namespace mesos {

#endif //  __COMMON_RESOURCE_QUANTITIES_HPP__
