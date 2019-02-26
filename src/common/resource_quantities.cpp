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

#include <string>
#include <vector>

#include <mesos/values.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/numify.hpp>

#include "common/resource_quantities.hpp"

using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

  // This function tries to be consistent with `Resources::fromSimpleString()`.
  // We trim the whitespace around the pair and in the number but whitespace in
  // "c p us:10" are preserved and will be parsed to {"c p us", 10}.
Try<ResourceQuantities> ResourceQuantities::fromString(const string& text)
{
  ResourceQuantities result;

  foreach (const string& token, strings::tokenize(text, ";")) {
    vector<string> pair = strings::tokenize(token, ":");
    if (pair.size() != 2) {
      return Error("Failed to parse '" + token + "': missing or extra ':'");
    }

    Try<Value> value = values::parse(pair[1]);
    if (value.isError()) {
      return Error(
          "Failed to parse '" + pair[1] + "' to quantity: " + value.error());
    }

    if (value->type() != Value::SCALAR) {
      return Error(
          "Failed to parse '" + pair[1] + "' to quantity:"
          " only scalar values are allowed");
    }

    if (value->scalar().value() < 0) {
      return Error(
          "Failed to parse '" + pair[1] + "' to quantity:"
          " negative values are not allowed");
    } else if (value->scalar().value() > 0) {
      result.add(strings::trim(pair[0]), value->scalar());
    }
    // Zero value is silently dropped.
  }

  return result;
}


ResourceQuantities ResourceQuantities::fromScalarResources(
  const Resources& resources)
{
  ResourceQuantities result;

  foreach (const Resource& resource, resources) {
    CHECK_EQ(Value::SCALAR, resource.type()) << " Resources: " << resources;

    result.add(resource.name(), resource.scalar());
  }

  return result;
}


ResourceQuantities::ResourceQuantities()
{
  // Pre-reserve space for first-class resources.
  // [cpus, disk, gpus, mem, ports]
  quantities.reserve(5u);
}


ResourceQuantities::const_iterator ResourceQuantities::begin()
{
  return static_cast<const std::vector<std::pair<std::string, Value::Scalar>>&>(
             quantities)
    .begin();
}


ResourceQuantities::const_iterator ResourceQuantities::end()
{
  return static_cast<const std::vector<std::pair<std::string, Value::Scalar>>&>(
             quantities)
    .end();
}


Value::Scalar ResourceQuantities::get(const string& name) const
{
  // Don't bother binary searching since
  // we don't expect a large number of elements.
  foreach (auto& quantity, quantities) {
    if (quantity.first == name) {
      return quantity.second;
    } else if (quantity.first > name) {
      // We can return early since we keep names in alphabetical order.
      break;
    }
  }

  return Value::Scalar();
}


ResourceQuantities& ResourceQuantities::operator+=(
    const ResourceQuantities& right)
{
  size_t leftIndex = 0u;
  size_t rightIndex = 0u;

  // Since quantities are sorted in alphabetical order, we can walk them
  // at the same time.
  while (leftIndex < size() && rightIndex < right.size()) {
    pair<string, Value::Scalar>& left_ = quantities.at(leftIndex);
    const pair<string, Value::Scalar>& right_ = right.quantities.at(rightIndex);

    if (left_.first < right_.first) {
      // Item exists in the left but not in the right.
      ++leftIndex;
    } else if (left_.first > right_.first) {
      // Item exists in the right but not in the left.
      // Insert absent entries in the alphabetical order.
      quantities.insert(quantities.begin() + leftIndex, right_);
      ++leftIndex;
      ++rightIndex;
    } else {
      // Item exists in both left and right.
      left_.second += right_.second;
      ++leftIndex;
      ++rightIndex;
    }
  }

  // Copy the remaining items in `right`.
  while (rightIndex < right.size()) {
    quantities.push_back(right.quantities.at(rightIndex));
    ++rightIndex;
  }

  return *this;
}


ResourceQuantities& ResourceQuantities::operator-=(
    const ResourceQuantities& right)
{
  size_t leftIndex = 0u;
  size_t rightIndex = 0u;

  // Since quantities are sorted in alphabetical order, we can walk them
  // at the same time.
  while (leftIndex < size() && rightIndex < right.size()) {
    pair<string, Value::Scalar>& left_ = quantities.at(leftIndex);
    const pair<string, Value::Scalar>& right_ = right.quantities.at(rightIndex);

    if (left_.first < right_.first) {
      // Item exists in the left but not in the right.
      ++leftIndex;
    } else if (left_.first > right_.first) {
      // Item exists in the right but not in the left (i.e. 0), this
      // would result in a negative entry, so skip it.
      ++rightIndex;
    } else {
      // Item exists in both left and right.
      if (left_.second <= right_.second) {
        // Drop negative and zero entries.
        quantities.erase(quantities.begin() + leftIndex);
        ++rightIndex;
      } else {
        left_.second -= right_.second;
        ++leftIndex;
        ++rightIndex;
      }
    }
  }

  return *this;
}


void ResourceQuantities::add(const string& name, const Value::Scalar& scalar)
{
  CHECK_GE(scalar, Value::Scalar());

  // Ignore adding zero.
  if (scalar == Value::Scalar()) {
    return;
  }

  // Find the location to insert while maintaining
  // alphabetical ordering. Don't bother binary searching
  // since we don't expect a large number of quantities.
  auto it = quantities.begin();
  for (; it != quantities.end(); ++it) {
    if (it->first == name) {
      it->second += scalar;
      return;
    }

    if (it->first > name) {
      break;
    }
  }

  it = quantities.insert(it, std::make_pair(name, scalar));
}


} // namespace internal {
} // namespace mesos {
