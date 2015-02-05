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

#include <mesos/values.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

using namespace mesos;
using namespace mesos::values;

using std::string;


TEST(ValuesTest, ValidInput)
{
  // Test parsing scalar type.
  Try<Value> result1 = parse("45.55");
  ASSERT_SOME(result1);
  ASSERT_EQ(Value::SCALAR, result1.get().type());
  EXPECT_EQ(45.55, result1.get().scalar().value());

  // Test parsing ranges type.
  Try<Value> result2 = parse("[10000-20000, 30000-50000]");
  ASSERT_SOME(result2);
  ASSERT_EQ(Value::RANGES, result2.get().type());
  EXPECT_EQ(2, result2.get().ranges().range_size());
  EXPECT_EQ(10000u, result2.get().ranges().range(0).begin());
  EXPECT_EQ(20000u, result2.get().ranges().range(0).end());
  EXPECT_EQ(30000u, result2.get().ranges().range(1).begin());
  EXPECT_EQ(50000u, result2.get().ranges().range(1).end());

  // Test parsing set type.
  Try<Value> result3 = parse("{sda1, sda2}");
  ASSERT_SOME(result3);
  ASSERT_EQ(Value::SET, result3.get().type());
  ASSERT_EQ(2, result3.get().set().item_size());
  EXPECT_EQ("sda1", result3.get().set().item(0));
  EXPECT_EQ("sda2", result3.get().set().item(1));

  // Test parsing text type.
  Try<Value> result4 = parse("123abc,s");
  ASSERT_SOME(result4);
  ASSERT_EQ(Value::TEXT, result4.get().type());
  ASSERT_EQ("123abc,s", result4.get().text().value());
}


TEST(ValuesTest, InvalidInput)
{
  // Test when '{' doesn't match.
  EXPECT_ERROR(parse("{aa,b}}"));

  // Test when '[' doesn't match.
  EXPECT_ERROR(parse("[1-2]]"));

  // Test when range is not numeric.
  EXPECT_ERROR(parse("[1-2b]"));

  // Test when giving empty string.
  EXPECT_ERROR(parse("  "));
}


TEST(ValuesTest, SetSubtraction)
{
  Value::Set set1 = parse("{sda1, sda2, sda3}").get().set();
  Value::Set set2 = parse("{sda2, sda3}").get().set();
  Value::Set set3 = parse("{sda4}").get().set();

  set1 -= set2;

  EXPECT_EQ(set1, parse("{sda1}").get().set());

  set3 -= set1;

  EXPECT_EQ(set3, parse("{sda4}").get().set());
}
