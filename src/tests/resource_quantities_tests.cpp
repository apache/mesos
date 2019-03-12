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

#include <gtest/gtest.h>

#include <stout/check.hpp>
#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include <mesos/values.hpp>

#include "common/resource_quantities.hpp"

using std::pair;
using std::string;
using std::vector;

using mesos::internal::ResourceQuantities;

namespace mesos {
namespace internal {
namespace tests {


static vector<pair<string, double>> toVector(
  const ResourceQuantities& quantities)
{
  vector<pair<string, double>> result;

  foreach (auto&& quantity, quantities) {
    result.push_back(std::make_pair(quantity.first, quantity.second.value()));
  }

  return result;
}


TEST(QuantitiesTest, FromStringValid)
{
  // A single resource.
  ResourceQuantities resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10"));
  vector<pair<string, double>> expected = {{"cpus", 10}};
  EXPECT_EQ(expected, toVector(resourceQuantities));

  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:3.14"));
  expected = {{"cpus", 3.14}};
  EXPECT_EQ(expected, toVector(resourceQuantities));

  // Whitespace is trimmed.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString(" cpus : 3.14 ; disk : 10 "));
  expected = {{"cpus", 3.14}, {"disk", 10}};
  EXPECT_EQ(expected, toVector(resourceQuantities));

  // Zero value.
  resourceQuantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:0"));
  EXPECT_EQ(0u, resourceQuantities.size());

  // Two resources.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10.5;ports:1"));
  expected = {{"cpus", 10.5}, {"ports", 1}};
  EXPECT_EQ(expected, toVector(resourceQuantities));

  // Two resources with names out of alphabetical order.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("ports:3;cpus:10.5"));
  expected = {{"cpus", 10.5}, {"ports", 3}};
  EXPECT_EQ(expected, toVector(resourceQuantities));

  // Duplicate resource names.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("ports:3;cpus:1;cpus:10.5"));
  expected = {{"cpus", 11.5}, {"ports", 3}};
  EXPECT_EQ(expected, toVector(resourceQuantities));
}


TEST(QuantitiesTest, FromStringInvalid)
{
  // Invalid scalar.
  Try<ResourceQuantities> resourceQuantities =
    ResourceQuantities::fromString("cpus:a10");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:3.14c");
  EXPECT_ERROR(resourceQuantities);

  // Missing semicolon.
  resourceQuantities = ResourceQuantities::fromString("ports:3,cpus:1");
  EXPECT_ERROR(resourceQuantities);

  // Negative value.
  resourceQuantities = ResourceQuantities::fromString("ports:3,cpus:-1");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:nan");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:-nan");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:inf");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:-inf");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:infinity");
  EXPECT_ERROR(resourceQuantities);

  resourceQuantities = ResourceQuantities::fromString("cpus:-infinity");
  EXPECT_ERROR(resourceQuantities);
}


TEST(QuantitiesTest, FromScalarResources)
{
  // Empty resources.
  ResourceQuantities quantities =
    ResourceQuantities::fromScalarResources(Resources());
  EXPECT_EQ(0u, quantities.size());

  // Result entries are ordered alphabetically.
  quantities = ResourceQuantities::fromScalarResources(
    CHECK_NOTERROR(Resources::parse("cpus:1;mem:512;disk:800")));
  vector<pair<string, double>> expected = {
    {"cpus", 1}, {"disk", 800}, {"mem", 512}};
  EXPECT_EQ(expected, toVector(quantities));
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
