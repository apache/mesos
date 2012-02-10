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

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "master/master.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using std::ostringstream;
using std::string;


TEST(ResourcesTest, Parsing)
{
  Resource cpus = Resources::parse("cpus", "45.55");
  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());

  Resource ports = Resources::parse("ports", "[10000-20000, 30000-50000]");
  ASSERT_EQ(Value::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  Resource disks = Resources::parse("disks", "{sda1}");
  ASSERT_EQ(Value::SET, disks.type());
  ASSERT_EQ(1, disks.set().item_size());
  EXPECT_EQ("sda1", disks.set().item(0));

  Resources r1 = Resources::parse("cpus:45.55;"
                                  "ports:[10000-20000, 30000-50000];"
                                  "disks:{sda1}");

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, Printing)
{
  Resources r = Resources::parse("cpus:45.55;"
                                 "ports:[10000-20000, 30000-50000];"
                                 "disks:{sda1}");

  string output = "cpus=45.55; ports=[10000-20000, 30000-50000]; disks={sda1}";

  ostringstream oss;
  oss << r;

  // TODO(benh): This test is a bit strict because it implies the
  // ordering of things (e.g., the ordering of resources and the
  // ordering of ranges). We should really just be checking for the
  // existance of certain substrings in the output.

  EXPECT_EQ(output, oss.str());
}


TEST(ResourcesTest, InitializedIsEmpty)
{
  Resources r;
  EXPECT_EQ(0, r.size());
}


TEST(ResourcesTest, BadResourcesNotAllocatable)
{
  Resource cpus;
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(1);
  Resources r;
  r += cpus;
  EXPECT_EQ(0, r.allocatable().size());
  cpus.set_name("cpus");
  cpus.mutable_scalar()->set_value(0);
  r += cpus;
  EXPECT_EQ(0, r.allocatable().size());
}


TEST(ResourcesTest, ScalarEquals)
{
  Resource cpus = Resources::parse("cpus", "3");
  Resource mem =  Resources::parse("mem", "3072");

  Resources r1;
  r1 += cpus;
  r1 += mem;

  Resources r2;
  r2 += cpus;
  r2 += mem;

  EXPECT_EQ(2, r1.size());
  EXPECT_EQ(2, r2.size());
  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ScalarSubset)
{
  Resource cpus1 = Resources::parse("cpus", "1");
  Resource mem1 =  Resources::parse("mem", "3072");

  Resource cpus2 = Resources::parse("cpus", "1");
  Resource mem2 =  Resources::parse("mem", "4096");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, ScalarAddition)
{
  Resource cpus1 = Resources::parse("cpus", "1");
  Resource mem1 = Resources::parse("mem", "5");

  Resource cpus2 = Resources::parse("cpus", "2");
  Resource mem2 = Resources::parse("mem", "10");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources sum = r1 + r2;
  EXPECT_EQ(2, sum.size());
  EXPECT_EQ(3, sum.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(15, sum.get("mem", Value::Scalar()).value());

  Resources r = r1;
  r += r2;
  EXPECT_EQ(2, r.size());
  EXPECT_EQ(3, r.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(15, r.get("mem", Value::Scalar()).value());
}


TEST(ResourcesTest, ScalarSubtraction)
{
  Resource cpus1 = Resources::parse("cpus", "50");
  Resource mem1 = Resources::parse("mem", "4096");

  Resource cpus2 = Resources::parse("cpus", "0.5");
  Resource mem2 = Resources::parse("mem", "1024");

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources diff = r1 - r2;
  EXPECT_EQ(2, diff.size());
  EXPECT_EQ(49.5, diff.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(3072, diff.get("mem", Value::Scalar()).value());

  Resources r = r1;
  r -= r2;
  EXPECT_EQ(49.5, diff.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(3072, diff.get("mem", Value::Scalar()).value());

  r = r1;
  r -= r1;
  EXPECT_EQ(0, r.allocatable().size());
}


TEST(ResourcesTest, RangesEquals)
{
  Resource ports = Resources::parse("ports", "[20000-40000]");

  Resources r1;
  r1 += ports;

  Resources r2;
  r2 += ports;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, RangesSubset)
{
  Resource ports1 = Resources::parse("ports", "[2-2, 4-5]");
  Resource ports2 = Resources::parse("ports", "[1-10]");
  Resource ports3 = Resources::parse("ports", "[2-3]");
  Resource ports4 = Resources::parse("ports", "[1-2, 4-6]");

  EXPECT_EQ(2, ports1.ranges().range_size());
  EXPECT_EQ(1, ports2.ranges().range_size());
  EXPECT_EQ(1, ports3.ranges().range_size());
  EXPECT_EQ(2, ports4.ranges().range_size());

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  Resources r3;
  r3 += ports3;

  Resources r4;
  r4 += ports4;

  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
  EXPECT_FALSE(r1 <= r3);
  EXPECT_FALSE(r3 <= r1);
  EXPECT_TRUE(r3 <= r2);
  EXPECT_FALSE(r2 <= r3);
  EXPECT_TRUE(r1 <= r4);
  EXPECT_TRUE(r4 <= r2);
}


TEST(ResourcesTest, RangesAddition)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000, 21000-38000]");
  Resource ports2 = Resources::parse("ports", "[30000-50000, 10000-20000]");

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_EQ(1, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(10000, ranges.range(0).begin());
  EXPECT_EQ(50000, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000]");
  Resource ports2 = Resources::parse("ports", "[10000-20000, 30000-50000]");

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(20001, ranges.range(0).begin());
  EXPECT_EQ(29999, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction1)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]");
  Resource ports2 = Resources::parse("ports", "[50000-50001]");

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(50002, ranges.range(0).begin());
  EXPECT_EQ(60000, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction2)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]");
  Resource ports2 = Resources::parse("ports", "[50000-50000]");

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(50001, ranges.range(0).begin());
  EXPECT_EQ(60000, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction3)
{
  Resources resources = Resources::parse("ports:[50000-60000]");

  Resources resourcesOffered = Resources::parse("");
  Resources resourcesInUse = Resources::parse("ports:[50000-50001]");

  Resources resourcesFree = resources - (resourcesOffered + resourcesInUse);

  resourcesFree = resourcesFree.allocatable();

  EXPECT_EQ(1, resourcesFree.size());

  const Value::Ranges& ranges = resourcesFree.get("ports", Value::Ranges());

  EXPECT_EQ(1, ranges.range_size());
  EXPECT_EQ(50002, ranges.range(0).begin());
  EXPECT_EQ(60000, ranges.range(0).end());
}


TEST(ResourcesTest, RangesSubtraction4)
{
  Resources resources = Resources::parse("ports:[50000-60000]");

  Resources resourcesOffered;

  resourcesOffered += resources;

  resourcesOffered -= resources;

  EXPECT_EQ(1, resourcesOffered.size());

  const Value::Ranges& ranges = resourcesOffered.get("ports", Value::Ranges());

  EXPECT_EQ(0, ranges.range_size());
}


TEST(ResourcesTest, SetEquals)
{
  Resource disks = Resources::parse("disks", "{sda1}");

  Resources r1;
  r1 += disks;

  Resources r2;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, SetSubset)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2}");
  Resource disks2 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");

  Resources r1;
  r1 += disks1;

  Resources r2;
  r2 += disks2;

  EXPECT_EQ(1, r1.size());
  EXPECT_EQ(1, r2.size());
  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, SetAddition)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2,sda3}");
  Resource disks2 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");

  Resources r;
  r += disks1;
  r += disks2;

  EXPECT_EQ(1, r.size());

  const Value::Set& set = r.get("disks", Value::Set());

  EXPECT_EQ(4, set.item_size());
}


TEST(ResourcesTest, SetSubtraction)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2,sda3,sda4}");
  Resource disks2 = Resources::parse("disks", "{sda2,sda3,sda4}");

  Resources r;
  r += disks1;
  r -= disks2;

  EXPECT_EQ(1, r.size());

  const Value::Set& set = r.get("disks", Value::Set());

  EXPECT_EQ(1, set.item_size());
  EXPECT_EQ("sda1", set.item(0));
}

TEST(ResourcesTest, EmptyUnequal)
{
  Resources empty = Resources::parse("");
  Resources cpus2 = Resources::parse("cpus:2");

  EXPECT_FALSE(empty == cpus2);
}
