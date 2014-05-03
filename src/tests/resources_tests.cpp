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

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>

#include "master/master.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using std::ostringstream;
using std::pair;
using std::string;


TEST(ResourcesTest, Parsing)
{
  Resource cpus = Resources::parse("cpus", "45.55", "*").get();
  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());

  Resource ports = Resources::parse("ports",
                                    "[10000-20000, 30000-50000]",
                                    "*").get();

  ASSERT_EQ(Value::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  Resource disks = Resources::parse("disks", "{sda1}", "*").get();
  ASSERT_EQ(Value::SET, disks.type());
  ASSERT_EQ(1, disks.set().item_size());
  EXPECT_EQ("sda1", disks.set().item(0));

  Resources r1 = Resources::parse("cpus:45.55;"
                                  "ports:[10000-20000, 30000-50000];"
                                  "disks:{sda1}").get();

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ParsingWithRoles)
{
  Resources parse1 = Resources::parse("cpus(role1):2;mem(role1):3").get();

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);
  cpus.set_role("role1");

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Value::SCALAR);
  mem.mutable_scalar()->set_value(3);
  mem.set_role("role1");

  Resources resources1;
  resources1 += cpus;
  resources1 += mem;

  EXPECT_EQ(parse1, resources1);

  EXPECT_EQ(resources1, Resources::parse(stringify(resources1)).get());

  Resources parse2 =
    Resources::parse("cpus(role1):2.5;ports(role2):[0-100]").get();

  Resource cpus2;
  cpus2.set_name("cpus");
  cpus2.set_type(Value::SCALAR);
  cpus2.mutable_scalar()->set_value(2.5);
  cpus2.set_role("role1");

  Resource ports;
  ports.set_name("ports");
  ports.set_type(Value::RANGES);
  Value::Range* range = ports.mutable_ranges()->add_range();
  range->set_begin(0);
  range->set_end(100);
  ports.set_role("role2");

  Resources resources2;
  resources2 += ports;
  resources2 += cpus2;

  EXPECT_EQ(parse2, resources2);

  EXPECT_EQ(resources2, Resources::parse(stringify(resources2)).get());

  Resources parse3 =
    Resources::parse("cpus:2.5;ports(role2):[0-100]", "role1").get();

  EXPECT_EQ(parse2, parse3);
}


TEST(ResourcesTest, ParseError)
{
  // Missing colon.
  EXPECT_ERROR(Resources::parse("cpus(role1)"));

  // Mismatched parentheses.
  EXPECT_ERROR(Resources::parse("cpus(role1:1"));

  EXPECT_ERROR(Resources::parse("cpus)(role1:1"));
}


TEST(ResourcesTest, Resources)
{
  Resources r = Resources::parse("cpus:45.55;"
                                 "mem:1024;"
                                 "ports:[10000-20000, 30000-50000];"
                                 "disk:512").get();

  EXPECT_SOME_EQ(45.55, r.cpus());
  EXPECT_SOME_EQ(Megabytes(1024), r.mem());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());

  EXPECT_SOME(r.ports());
  ostringstream ports;
  ports << r.ports().get();

  EXPECT_EQ("[10000-20000, 30000-50000]", ports.str());

  r = Resources::parse("cpus:45.55;disk:512").get();
  EXPECT_SOME_EQ(45.55, r.cpus());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());
  EXPECT_TRUE(r.mem().isNone());
  EXPECT_TRUE(r.ports().isNone());
}


TEST(ResourcesTest, Printing)
{
  Resources r = Resources::parse("cpus:45.55;"
                                 "ports:[10000-20000, 30000-50000];"
                                 "disks:{sda1}").get();

  string output =
    "cpus(*):45.55; ports(*):[10000-20000, 30000-50000]; disks(*):{sda1}";

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
  EXPECT_EQ(0u, r.size());
}


TEST(ResourcesTest, BadResourcesNotAllocatable)
{
  Resource cpus;
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(1);
  Resources r;
  r += cpus;
  EXPECT_EQ(0u, r.allocatable().size());
  cpus.set_name("cpus");
  cpus.mutable_scalar()->set_value(0);
  r += cpus;
  EXPECT_EQ(0u, r.allocatable().size());
}


TEST(ResourcesTest, ScalarEquals)
{
  Resource cpus = Resources::parse("cpus", "3", "*").get();
  Resource mem =  Resources::parse("mem", "3072", "*").get();

  Resources r1;
  r1 += cpus;
  r1 += mem;

  Resources r2;
  r2 += cpus;
  r2 += mem;

  EXPECT_EQ(2u, r1.size());
  EXPECT_EQ(2u, r2.size());
  EXPECT_EQ(r1, r2);

  Resource cpus1 = Resources::parse("cpus", "3", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  EXPECT_NE(cpus1, cpus2);
}


TEST(ResourcesTest, ScalarSubset)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1 =  Resources::parse("mem", "3072", "*").get();

  Resource cpus2 = Resources::parse("cpus", "1", "*").get();
  Resource mem2 =  Resources::parse("mem", "4096", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, ScalarSubset2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "1", "role2").get();

  Resources r1;
  r1 += cpus1;

  Resources r2;
  r2 += cpus2;

  EXPECT_FALSE(cpus1 <= cpus2);
  EXPECT_FALSE(cpus2 <= cpus1);

  Resource cpus3 = Resources::parse("cpus", "3", "role1").get();

  Resources r3;
  r3 += cpus3;

  EXPECT_FALSE(r3 <= r1);
  EXPECT_FALSE(r3 <= r2);
  EXPECT_FALSE(r2 <= r3);
  EXPECT_LE(r1, r3);
}


TEST(ResourcesTest, ScalarAddition)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1 = Resources::parse("mem", "5", "*").get();

  Resource cpus2 = Resources::parse("cpus", "2", "*").get();
  Resource mem2 = Resources::parse("mem", "10", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources sum = r1 + r2;
  EXPECT_EQ(2u, sum.size());
  EXPECT_EQ(3, sum.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(15, sum.get("mem", Value::Scalar()).value());

  Resources r = r1;
  r += r2;
  EXPECT_EQ(2u, r.size());
  EXPECT_EQ(3, r.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(15, r.get("mem", Value::Scalar()).value());
}


TEST(ResourcesTest, ScalarAddition2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "5", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += cpus2;

  Resources r2;
  r2 += cpus3;

  Resources sum = r1 + r2;
  EXPECT_EQ(2u, sum.size());
  EXPECT_EQ(9, sum.cpus().get());
  EXPECT_EQ(sum, Resources::parse("cpus(role1):6;cpus(role2):3").get());
}


TEST(ResourcesTest, ScalarSubtraction)
{
  Resource cpus1 = Resources::parse("cpus", "50", "*").get();
  Resource mem1 = Resources::parse("mem", "4096", "*").get();

  Resource cpus2 = Resources::parse("cpus", "0.5", "*").get();
  Resource mem2 = Resources::parse("mem", "1024", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources diff = r1 - r2;
  EXPECT_EQ(2u, diff.size());
  EXPECT_EQ(49.5, diff.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(3072, diff.get("mem", Value::Scalar()).value());

  Resources r = r1;
  r -= r2;
  EXPECT_EQ(49.5, diff.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(3072, diff.get("mem", Value::Scalar()).value());

  r = r1;
  r -= r1;
  EXPECT_EQ(0u, r.allocatable().size());
}


TEST(ResourcesTest, ScalarSubtraction2)
{
  Resource cpus1 = Resources::parse("cpus", "5", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "1", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += cpus2;

  Resources r2;
  r2 += cpus3;

  Resources diff = r1 - r2;
  EXPECT_EQ(2u, diff.size());
  EXPECT_EQ(7, diff.cpus().get());
  EXPECT_EQ(diff, Resources::parse("cpus(role1):4;cpus(role2):3").get());
}


TEST(ResourcesTest, RangesEquals)
{
  Resource ports1 = Resources::parse("ports", "[20-40]", "*").get();
  Resource ports2 =
    Resources::parse("ports", "[20-30, 31-39, 40-40]", "*").get();

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, RangesSubset)
{
  Resource ports1 = Resources::parse("ports", "[2-2, 4-5]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports3 = Resources::parse("ports", "[2-3]", "*").get();
  Resource ports4 = Resources::parse("ports", "[1-2, 4-6]", "*").get();
  Resource ports5 = Resources::parse("ports", "[1-4, 5-5]", "*").get();

  EXPECT_EQ(2, ports1.ranges().range_size());
  EXPECT_EQ(1, ports2.ranges().range_size());
  EXPECT_EQ(1, ports3.ranges().range_size());
  EXPECT_EQ(2, ports4.ranges().range_size());
  EXPECT_EQ(2, ports5.ranges().range_size());

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  Resources r3;
  r3 += ports3;

  Resources r4;
  r4 += ports4;

  Resources r5;
  r5 += ports5;

  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
  EXPECT_FALSE(r1 <= r3);
  EXPECT_FALSE(r3 <= r1);
  EXPECT_TRUE(r3 <= r2);
  EXPECT_FALSE(r2 <= r3);
  EXPECT_TRUE(r1 <= r4);
  EXPECT_TRUE(r4 <= r2);
  EXPECT_TRUE(r1 <= r5);
  EXPECT_FALSE(r5 <= r1);
}


TEST(ResourcesTest, RangesAddition)
{
  Resource ports1 =
    Resources::parse("ports", "[20000-40000, 21000-38000]", "*").get();
  Resource ports2 =
    Resources::parse("ports", "[30000-50000, 10000-20000]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[10000-50000]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesAddition2)
{
  Resource ports1 = Resources::parse("ports", "[1-10, 5-30, 50-60]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-65, 70-80]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-65, 70-80]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesAdditon3)
{
  Resource ports1 = Resources::parse("ports", "[1-2]", "*").get();
  Resource ports2 = Resources::parse("ports", "[3-4]", "*").get();
  Resource ports3 = Resources::parse("ports", "[7-8]", "*").get();
  Resource ports4 = Resources::parse("ports", "[5-6]", "*").get();

  Resources r1;
  r1 += ports1;
  r1 += ports2;

  EXPECT_EQ(1u, r1.size());

  const Value::Ranges& ranges = r1.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-4]").get().ranges(), ranges);

  Resources r2;
  r2 += ports3;
  r2 += ports4;

  EXPECT_EQ(1u, r2.size());

  const Value::Ranges& ranges2 = r2.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[5-8]").get().ranges(), ranges2);

  r2 += r1;

  EXPECT_EQ(1u, r2.size());

  const Value::Ranges& ranges3 = r2.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-8]").get().ranges(), ranges3);
}


TEST(ResourcesTest, RangesAddition4)
{
  Resource ports1 =
    Resources::parse("ports", "[1-4, 9-10, 20-22, 26-30]", "*").get();
  Resource ports2 = Resources::parse("ports", "[5-8, 23-25]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-10, 20-30]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction)
{
  Resource ports1 = Resources::parse("ports", "[20000-40000]", "*").get();
  Resource ports2 =
    Resources::parse("ports", "[10000-20000, 30000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[20001-29999]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction1)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50001]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[50002-60000]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction2)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[50001-60000]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction3)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered = Resources::parse("").get();
  Resources resourcesInUse = Resources::parse("ports:[50000-50001]").get();

  Resources resourcesFree = resources - (resourcesOffered + resourcesInUse);

  resourcesFree = resourcesFree.allocatable();

  EXPECT_EQ(1u, resourcesFree.size());

  const Value::Ranges& ranges = resourcesFree.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[50002-60000]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction4)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered;

  resourcesOffered += resources;

  resourcesOffered -= resources;

  EXPECT_EQ(0u, resourcesOffered.size());

  const Value::Ranges& ranges = resourcesOffered.get("ports", Value::Ranges());

  EXPECT_EQ(0, ranges.range_size());
}


TEST(ResourcesTest, RangesSubtraction5)
{
  Resource ports1 =
    Resources::parse("ports", "[1-10, 20-30, 40-50]", "*").get();
  Resource ports2 = Resources::parse("ports", "[2-9, 15-45, 48-50]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-1, 10-10, 46-47]").get().ranges(), ranges);
}


TEST(ResourcesTest, RangesSubtraction6)
{
  Resource ports1 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports2 = Resources::parse("ports", "[11-20]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_EQ(1u, r.size());

  const Value::Ranges& ranges = r.get("ports", Value::Ranges());

  EXPECT_EQ(values::parse("[1-10]").get().ranges(), ranges);
}


TEST(ResourcesTest, SetEquals)
{
  Resource disks = Resources::parse("disks", "{sda1}", "*").get();

  Resources r1;
  r1 += disks;

  Resources r2;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, SetSubset)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2}", "*").get();
  Resource disks2 =
    Resources::parse("disks", "{sda1,sda3,sda4,sda2}", "*").get();

  Resources r1;
  r1 += disks1;

  Resources r2;
  r2 += disks2;

  EXPECT_EQ(1u, r1.size());
  EXPECT_EQ(1u, r2.size());
  EXPECT_TRUE(r1 <= r2);
  EXPECT_FALSE(r2 <= r1);
}


TEST(ResourcesTest, SetAddition)
{
  Resource disks1 = Resources::parse("disks", "{sda1,sda2,sda3}", "*").get();
  Resource disks2 =
    Resources::parse("disks", "{sda1,sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r += disks2;

  EXPECT_EQ(1u, r.size());

  const Value::Set& set = r.get("disks", Value::Set());

  EXPECT_EQ(4, set.item_size());
}


TEST(ResourcesTest, SetSubtraction)
{
  Resource disks1 =
    Resources::parse("disks", "{sda1,sda2,sda3,sda4}", "*").get();
  Resource disks2 = Resources::parse("disks", "{sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r -= disks2;

  EXPECT_EQ(1u, r.size());

  const Value::Set& set = r.get("disks", Value::Set());

  EXPECT_EQ(1, set.item_size());
  EXPECT_EQ("sda1", set.item(0));
}


TEST(ResourcesTest, EmptyUnequal)
{
  Resources empty = Resources::parse("").get();
  Resources cpus2 = Resources::parse("cpus:2").get();

  EXPECT_FALSE(empty == cpus2);
}


TEST(ResourcesTest, FlattenRoles)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "2", "role2").get();
  Resource mem1 = Resources::parse("mem", "5", "role1").get();

  Resources r;
  r += cpus1;
  r += cpus2;
  r += mem1;

  EXPECT_EQ(r.flatten(), Resources::parse("cpus:3;mem:5").get());
}


TEST(ResourcesTest, Find)
{
  Resources resources1 =
    Resources::parse("cpus(role1):2;mem(role1):10;cpus:4;mem:20").get();
  Resources toFind1 = Resources::parse("cpus:3;mem:15").get();

  Resources found1 = resources1.find(toFind1, "role1").get();
  Resources expected1 =
    Resources::parse("cpus(role1):2;mem(role1):10;cpus:1;mem:5").get();
  EXPECT_EQ(found1, expected1);

  Resources resources2 =
    Resources::parse("cpus(role1):1;mem(role1):5;cpus(role2):2;"
                     "mem(role2):8;cpus:1;mem:7").get();
  Resources toFind2 = Resources::parse("cpus:3;mem:15").get();

  Resources found2 = resources2.find(toFind2, "role1").get();
  Resources expected2 =
    Resources::parse("cpus(role1):1;mem(role1):5;cpus:1;mem:7;"
                     "cpus(role2):1;mem(role2):3").get();
  EXPECT_EQ(found2, expected2);

  Resources resources3 =
    Resources::parse("cpus(role1):5;mem(role1):5;cpus:5;mem:5").get();
  Resources toFind3 = Resources::parse("cpus:6;mem:6").get();

  Resources found3 = resources3.find(toFind3).get();
  Resources expected3 =
    Resources::parse("cpus:5;mem:5;cpus(role1):1;mem(role1):1").get();

  EXPECT_EQ(found3, expected3);

  Resources resources4 = Resources::parse("cpus(role1):1;mem(role1):1").get();
  Resources toFind4 = Resources::parse("cpus:2;mem:2").get();

  EXPECT_NONE(resources4.find(toFind1, "role1"));
}
