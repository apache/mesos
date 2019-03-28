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
#include <vector>

#include <mesos/mesos.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace internal {


// An efficient collection of resource quantities. All values are guaranteed
// to be positive and finite.
//
// E.g. [("cpus", 4.5), ("gpus", 0.1), ("ports", 1000)]
//
// Absent resource entries imply there is no (zero) such resources.
//
// TODO(mzhu): Add `class ResourceLimits` where absence means infinite amount
// of such resources.
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

  ResourceQuantities();

  ResourceQuantities(const ResourceQuantities& that) = default;
  ResourceQuantities(ResourceQuantities&& that) = default;

  ResourceQuantities& operator=(const ResourceQuantities& that) = default;
  ResourceQuantities& operator=(ResourceQuantities&& that) = default;

  typedef std::vector<std::pair<std::string, Value::Scalar>>::const_iterator
    iterator;
  typedef std::vector<std::pair<std::string, Value::Scalar>>::const_iterator
    const_iterator;

  // NOTE: Non-`const` `iterator`, `begin()` and `end()` are __intentionally__
  // defined with `const` semantics in order to prevent mutation during
  // iteration. Mutation is allowed with the `[]` operator.
  const_iterator begin();
  const_iterator end();

  const_iterator begin() const { return quantities.begin(); }
  const_iterator end() const { return quantities.end(); }

  size_t size() const { return quantities.size(); };

  // Returns the quantity scalar value if a quantity with the given name.
  // If the given name is absent, return zero.
  Value::Scalar get(const std::string& name) const;

  bool operator==(const ResourceQuantities& quantities) const;
  bool operator!=(const ResourceQuantities& quantities) const;

  ResourceQuantities& operator+=(const ResourceQuantities& quantities);
  ResourceQuantities& operator-=(const ResourceQuantities& quantities);

private:
  void add(const std::string& name, const Value::Scalar& scalar);

  // List of name quantity pairs sorted by name.
  // Arithmetic and comparison operations benefit from this sorting.
  std::vector<std::pair<std::string, Value::Scalar>> quantities;
};


} // namespace internal {
} // namespace mesos {

#endif //  __COMMON_RESOURCE_QUANTITIES_HPP__
