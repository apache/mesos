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


// An efficient collection of resource quantities.
//
// E.g. [("cpus", 4.5), ("gpus", 0), ("ports", 1000)]
//
// We provide map-like semantics: [] operator for inserting/retrieving values,
// keys are unique, and iteration is sorted.
//
// Notes on handling negativity and arithmetic operations: during construction,
// `Value::Scalar` arguments must be non-negative and finite. Invalid arguments
// will result in an error where `Try` is returned. Once constructed, users can
// use `[]` operator to mutate values directly. No member methods are provided
// for arithmetic operations. Users should be mindful of producing negatives
// when mutating the `Value::Scalar` directly.
//
// An alternative design for this class was to provide addition and
// subtraction functions as opposed to a map interface. However, this
// leads to some un-obvious semantics around presence of zero values
// (will zero entries be automatically removed?) and handling of negatives
// (will negatives trigger a crash? will they be converted to zero? Note
// that Value::Scalar should be non-negative but there is currently no
// enforcement of this by Value::Scalar arithmetic operations; we probably
// want all Value::Scalar operations to go through the same negative
// handling). To avoid the confusion, we provided a map like interface
// which produces clear semantics: insertion works like maps, and the
// user is responsible for performing arithmetic operations on the values
// (using the already provided Value::Scalar arithmetic overloads).
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

  // Returns the quantity scalar value if a quantity with the given name
  // exists (even if the quantity is zero), otherwise return `None()`.
  Option<Value::Scalar> get(const std::string& name) const;

  // Like std::map, returns a reference to the quantity with the given name.
  // If no quantity exists with the given name, a new entry will be created.
  Value::Scalar& operator[](const std::string& name);

private:
  // List of name quantity pairs sorted by name.
  // Arithmetic and comparison operations benefit from this sorting.
  std::vector<std::pair<std::string, Value::Scalar>> quantities;
};


} // namespace internal {
} // namespace mesos {

#endif //  __COMMON_RESOURCE_QUANTITIES_HPP__
