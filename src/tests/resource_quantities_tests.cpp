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
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <mesos/resources.hpp>
#include <mesos/resource_quantities.hpp>
#include <mesos/values.hpp>

using std::pair;
using std::string;
using std::vector;

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


TEST(QuantitiesTest, FromResources)
{
  // Empty resources.
  ResourceQuantities quantities =
    ResourceQuantities::fromResources(Resources());
  EXPECT_EQ(0u, quantities.size());

  // Result entries are ordered alphabetically.
  quantities =
    ResourceQuantities::fromResources(
        CHECK_NOTERROR(Resources::parse(
            "cpus:1;mem:512;ports:[5000-6000];zones:{a,b};disk:800")));
  vector<pair<string, double>> expected = {
    {"cpus", 1}, {"disk", 800}, {"mem", 512}, {"ports", 1001}, {"zones", 2}};
  EXPECT_EQ(expected, toVector(quantities));
}


TEST(QuantitiesTest, Addition)
{
  // Empty quantity:
  // "cpus:10" + [ ] = "cpus:10"
  ResourceQuantities quantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10"));
  quantities += ResourceQuantities();
  vector<pair<string, double>> expected = {{"cpus", 10}};
  EXPECT_EQ(expected, toVector(quantities));

  // Same name entries:
  // "cpus:10" + "cpus:0.1" = "cpus:10.1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10"));
  quantities += CHECK_NOTERROR(ResourceQuantities::fromString("cpus:0.1"));
  expected = {{"cpus", 10.1}};
  EXPECT_EQ(expected, toVector(quantities));

  // Different name entries, insert at head:
  // "cpus:10.1" + "alphas:1" = "alphas:1;cpus:10.1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10.1"));
  quantities += CHECK_NOTERROR(ResourceQuantities::fromString("alphas:1"));
  expected = {{"alphas", 1}, {"cpus", 10.1}};
  EXPECT_EQ(expected, toVector(quantities));

  // Different name entries, insert at tail:
  // "cpus:10.1" + "mem:1" = "cpus:10.1;mem:1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10.1"));
  quantities += CHECK_NOTERROR(ResourceQuantities::fromString("mem:1"));
  expected = {{"cpus", 10.1}, {"mem", 1}};
  EXPECT_EQ(expected, toVector(quantities));

  // Mixed: "alphas:1;cpus:10.1" + "cpus:1;mem:100" =
  // "alphas:1;cpus:11.1;mem:100"
  quantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("alphas:1;cpus:10.1"));
  quantities +=
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:100"));
  expected = {{"alphas", 1}, {"cpus", 11.1}, {"mem", 100}};
  EXPECT_EQ(expected, toVector(quantities));
}

TEST(QuantitiesTest, Subtraction)
{
  // Empty subtrahend:
  // [ ] - "cpus:1" = [ ]
  ResourceQuantities quantities{};
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  EXPECT_EQ(0u, quantities.size());

  // Empty minuend:
  // "cpus:1" - [ ] = "cpus:1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= ResourceQuantities();
  vector<pair<string, double>> expected = {{"cpus", 1}};
  EXPECT_EQ(expected, toVector(quantities));

  // Same name entry, positive result is retained:
  // "cpus:1" - "cpus:0.4" = "cpus:0.6"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("cpus:0.4"));
  expected = {{"cpus", 0.6}};
  EXPECT_EQ(expected, toVector(quantities));

  // Same name entry, zero is dropped:
  // "cpus:1" - "cpus:1" = [ ]
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  EXPECT_EQ(0u, quantities.size());

  // Same name entry, negative entry is dropped:
  // "cpus:1" - "cpus:1.4" = [ ]
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1.4"));
  EXPECT_EQ(0u, quantities.size());

  // Different name entry:
  // "cpus:1" - "mem:100" = "cpus:1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("mem:100"));
  expected = {{"cpus", 1}};
  EXPECT_EQ(expected, toVector(quantities));

  // "cpus:1" - "alphas:1" = "cpus:1"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  quantities -= CHECK_NOTERROR(ResourceQuantities::fromString("alphas:1"));
  expected = {{"cpus", 1}};
  EXPECT_EQ(expected, toVector(quantities));

  // Mixed: "cpus:1;mem:100" - "alphas:1;cpus:0.5" = "cpus:0.5;mem:100"
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:100"));
  quantities -=
    CHECK_NOTERROR(ResourceQuantities::fromString("alphas:1;cpus:0.5"));
  expected = {{"cpus", 0.5}, {"mem", 100}};
  EXPECT_EQ(expected, toVector(quantities));
}


TEST(QuantitiesTest, Contains)
{
  ResourceQuantities empty{};
  EXPECT_TRUE(empty.contains(empty));

  ResourceQuantities some =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  EXPECT_TRUE(some.contains(empty));
  EXPECT_FALSE(empty.contains(some));

  // Self contains.
  some = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(some.contains(some));

  // Superset and subset.
  ResourceQuantities superset =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1;disk:1"));
  ResourceQuantities subset =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(superset.contains(subset));
  EXPECT_FALSE(subset.contains(superset));

  // Intersected sets.
  ResourceQuantities set1 =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  ResourceQuantities set2 =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;disk:1"));
  EXPECT_FALSE(set1.contains(set2));
  EXPECT_FALSE(set2.contains(set1));

  // Sets with no intersection.
  set1 = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  set2 = CHECK_NOTERROR(ResourceQuantities::fromString("gpu:1;disk:1"));
  EXPECT_FALSE(set1.contains(set2));
  EXPECT_FALSE(set2.contains(set1));

  // Same name, different scalars.
  superset = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:2;mem:2"));
  subset = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:2;mem:1"));
  EXPECT_TRUE(superset.contains(subset));
  EXPECT_FALSE(subset.contains(superset));
}


TEST(QuantitiesTest, Stringify)
{
  ResourceQuantities empty{};
  EXPECT_EQ("{}", stringify(empty));

  ResourceQuantities some =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1024"));

  EXPECT_EQ("cpus:1; mem:1024", stringify(some));
}


TEST(QuantitiesTest, Sum)
{
  ResourceQuantities empty{};
  ResourceQuantities cpus1 =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1"));
  ResourceQuantities cpus2 =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:2"));
  ResourceQuantities memory =
    CHECK_NOTERROR(ResourceQuantities::fromString("memory:1"));
  ResourceQuantities cpuAndMemory =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;memory:1"));

  hashmap<string, ResourceQuantities> quantitiesMap;
  vector<pair<string, double>> expected;
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));

  quantitiesMap["empty"] = empty;
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));

  quantitiesMap["cpus1"] = cpus1;
  expected = {{"cpus", 1}};
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));

  quantitiesMap["cpus2"] = cpus2;
  expected = {{"cpus", 3}};
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));

  quantitiesMap["memory"] = memory;
  expected = {{"cpus", 3}, {"memory", 1}};
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));

  quantitiesMap["cpuAndMemory"] = cpuAndMemory;
  expected = {{"cpus", 4}, {"memory", 2}};
  EXPECT_EQ(expected, toVector(ResourceQuantities::sum(quantitiesMap)));
}


static vector<pair<string, double>> toVector(
  const ResourceLimits& limits)
{
  vector<pair<string, double>> result;

  foreach (auto& limit, limits) {
    result.push_back(std::make_pair(limit.first, limit.second.value()));
  }

  return result;
}


// These are similar to `QuantitiesTest.FromStringValid` except when zero
// values are involved.
TEST(LimitsTest, FromStringValid)
{
  // A single resource.
  ResourceLimits resourceLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:10"));
  vector<pair<string, double>> expected = {{"cpus", 10}};
  EXPECT_EQ(expected, toVector(resourceLimits));

  resourceLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:3.14"));
  expected = {{"cpus", 3.14}};
  EXPECT_EQ(expected, toVector(resourceLimits));

  // Whitespace is trimmed.
  resourceLimits =
    CHECK_NOTERROR(ResourceLimits::fromString(" cpus : 3.14 ; disk : 10 "));
  expected = {{"cpus", 3.14}, {"disk", 10}};
  EXPECT_EQ(expected, toVector(resourceLimits));

  // Zero value is preserved.
  resourceLimits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:0"));
  expected = {{"cpus", 0}};
  EXPECT_EQ(expected, toVector(resourceLimits));

  // Two resources.
  resourceLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:10.5;ports:1"));
  expected = {{"cpus", 10.5}, {"ports", 1}};
  EXPECT_EQ(expected, toVector(resourceLimits));

  // Two resources with names out of alphabetical order.
  resourceLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("ports:3;cpus:10.5"));
  expected = {{"cpus", 10.5}, {"ports", 3}};
  EXPECT_EQ(expected, toVector(resourceLimits));
}


// These are identical to `QuantitiesTest.FromStringInvalid`.
TEST(LimitsTest, FromStringInvalid)
{
  // Invalid scalar.
  Try<ResourceLimits> resourceLimits =
    ResourceLimits::fromString("cpus:a10");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:3.14c");
  EXPECT_ERROR(resourceLimits);

  // Missing semicolon.
  resourceLimits = ResourceLimits::fromString("ports:3,cpus:1");
  EXPECT_ERROR(resourceLimits);

  // Negative value.
  resourceLimits = ResourceLimits::fromString("ports:3,cpus:-1");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:nan");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:-nan");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:inf");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:-inf");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:infinity");
  EXPECT_ERROR(resourceLimits);

  resourceLimits = ResourceLimits::fromString("cpus:-infinity");
  EXPECT_ERROR(resourceLimits);

  // Duplicate entries.
  resourceLimits = ResourceLimits::fromString("cpus:1;cpus:2");
  EXPECT_ERROR(resourceLimits);
}


TEST(LimitsTest, Contains)
{
  ResourceLimits infinite{};
  EXPECT_TRUE(infinite.contains(infinite));

  ResourceLimits finite = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1"));
  infinite = ResourceLimits();
  EXPECT_TRUE(infinite.contains(finite));
  EXPECT_FALSE(finite.contains(infinite));

  finite = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(finite.contains(finite));

  ResourceLimits moreLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1;disk:1"));
  ResourceLimits lessLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(lessLimits.contains(moreLimits));
  EXPECT_FALSE(moreLimits.contains(lessLimits));

  // Intersected sets.
  ResourceLimits limits1 =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  ResourceLimits limits2 =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;disk:1"));
  EXPECT_FALSE(limits1.contains(limits2));
  EXPECT_FALSE(limits2.contains(limits1));

  // Sets with no intersection.
  limits1 = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  limits2 = CHECK_NOTERROR(ResourceLimits::fromString("gpu:1;disk:1"));
  EXPECT_FALSE(limits1.contains(limits2));
  EXPECT_FALSE(limits2.contains(limits1));

  // Same name, different scalars.
  ResourceLimits higherLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:2;mem:2"));
  ResourceLimits lowerLimits =
    CHECK_NOTERROR(ResourceLimits::fromString("cpus:2;mem:1"));
  EXPECT_TRUE(higherLimits.contains(lowerLimits));
  EXPECT_FALSE(lowerLimits.contains(higherLimits));
}


TEST(LimitsTest, ContainsQuantities)
{
  ResourceLimits noLimit{};
  ResourceQuantities emptyQuantity{};
  EXPECT_TRUE(noLimit.contains(emptyQuantity));

  ResourceLimits limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1"));
  EXPECT_TRUE(limits.contains(emptyQuantity));

  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  ResourceQuantities quantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(limits.contains(quantities));

  // Superset and subset.
  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1;disk:1"));
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1"));
  EXPECT_TRUE(limits.contains(quantities));

  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  quantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;mem:1;disk:1"));
  EXPECT_TRUE(limits.contains(quantities));

  // Intersected sets.
  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:1;disk:1"));
  EXPECT_TRUE(limits.contains(quantities));

  // Sets with no intersection.
  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:1;mem:1"));
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("gpu:1;disk:1"));
  EXPECT_TRUE(limits.contains(quantities));

  // Same name, different scalars.
  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:2;mem:2"));
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:2;mem:1"));
  EXPECT_TRUE(limits.contains(quantities));

  limits = CHECK_NOTERROR(ResourceLimits::fromString("cpus:2;mem:1"));
  quantities = CHECK_NOTERROR(ResourceQuantities::fromString("cpus:2;mem:2"));
  EXPECT_FALSE(limits.contains(quantities));
}


TEST(LimitsTest, SubtractQuantities)
{
  auto limits = [](const string& resourceLimitsString) {
    return CHECK_NOTERROR(ResourceLimits::fromString(resourceLimitsString));
  };

  auto subtract = [](const string& resourceLimitsString,
                     const string& resourceQuantitiesString) {
    ResourceLimits limits =
      CHECK_NOTERROR(ResourceLimits::fromString(resourceLimitsString));
    ResourceQuantities quantities =
      CHECK_NOTERROR(ResourceQuantities::fromString(resourceQuantitiesString));

    return limits - quantities;
  };

  EXPECT_EQ(limits(""), subtract("", ""));
  EXPECT_EQ(limits(""), subtract("", "cpus:10"));

  EXPECT_EQ(limits("cpus:1"), subtract("cpus:1", ""));

  EXPECT_EQ(limits("cpus:0"), subtract("cpus:1", "cpus:1"));
  EXPECT_EQ(limits("cpus:0"), subtract("cpus:1", "cpus:2"));

  EXPECT_EQ(limits("cpus:0;mem:10"), subtract("cpus:1;mem:10", "cpus:1"));
  EXPECT_EQ(
      limits("cpus:0;mem:10"), subtract("cpus:1;mem:10", "cpus:1;disk:10"));

  EXPECT_EQ(
      limits("cpus:0;mem:5"),
      subtract("cpus:1;mem:10", "cpus:1;mem:5;disk:10"));
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
