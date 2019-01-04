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


TEST(QuantitiesTest, FromStringValid)
{
  // A single resource.
  ResourceQuantities resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10"));
  EXPECT_EQ(1u, resourceQuantities.size());
  EXPECT_EQ("cpus", resourceQuantities.begin()->first);
  EXPECT_DOUBLE_EQ(10, resourceQuantities.begin()->second.value());

  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:3.14"));
  EXPECT_EQ(1u, resourceQuantities.size());
  EXPECT_EQ("cpus", resourceQuantities.begin()->first);
  EXPECT_DOUBLE_EQ(3.14, resourceQuantities.begin()->second.value());

  // Zero value is retained.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:0"));
  EXPECT_EQ(1u, resourceQuantities.size());
  EXPECT_EQ("cpus", resourceQuantities.begin()->first);
  EXPECT_DOUBLE_EQ(0, resourceQuantities.begin()->second.value());

  // Whitespace is trimmed.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString(" cpus : 3.14 ; disk : 10 "));
  EXPECT_EQ(2u, resourceQuantities.size());
  auto it = resourceQuantities.begin();
  EXPECT_EQ("cpus", it->first);
  EXPECT_DOUBLE_EQ(3.14, it->second.value());
  ++it;
  EXPECT_EQ("disk", it->first);
  EXPECT_DOUBLE_EQ(10, it->second.value());

  // Two resources.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10.5;ports:1"));
  EXPECT_EQ(2u, resourceQuantities.size());
  it = resourceQuantities.begin();
  EXPECT_EQ("cpus", it->first);
  EXPECT_DOUBLE_EQ(10.5, it->second.value());
  ++it;
  EXPECT_EQ("ports", it->first);
  EXPECT_DOUBLE_EQ(1, it->second.value());

  // Two resources with names out of alphabetical order.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("ports:3;cpus:10.5"));
  EXPECT_EQ(2u, resourceQuantities.size());
  it = resourceQuantities.begin();
  EXPECT_EQ("cpus", it->first);
  EXPECT_DOUBLE_EQ(10.5, it->second.value());
  ++it;
  EXPECT_EQ("ports", it->first);
  EXPECT_DOUBLE_EQ(3, it->second.value());

  // Duplicate resource names.
  resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("ports:3;cpus:1;cpus:10.5"));
  EXPECT_EQ(2u, resourceQuantities.size());
  it = resourceQuantities.begin();
  EXPECT_EQ("cpus", it->first);
  EXPECT_DOUBLE_EQ(11.5, it->second.value());
  ++it;
  EXPECT_EQ("ports", it->first);
  EXPECT_DOUBLE_EQ(3, it->second.value());
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


TEST(QuantitiesTest, Insertion)
{
  ResourceQuantities resourceQuantities =
    CHECK_NOTERROR(ResourceQuantities::fromString("cpus:10"));

  resourceQuantities["disk"] = Value::Scalar();
  resourceQuantities["alpha"] = Value::Scalar();

  auto it = resourceQuantities.begin();
  EXPECT_EQ("alpha", it->first);
  EXPECT_DOUBLE_EQ(0, it->second.value());

  ++it;
  EXPECT_EQ("cpus", it->first);
  EXPECT_DOUBLE_EQ(10, it->second.value());

  ++it;
  EXPECT_EQ("disk", it->first);
  EXPECT_DOUBLE_EQ(0, it->second.value());
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
