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

using namespace mesos::internal::values;

using std::string;

namespace mesos {
  extern void coalesce(Value::Ranges* ranges);
  extern void coalesce(Value::Ranges* ranges, const Value::Range& range);
} // namespace mesos {

namespace mesos {
namespace internal {
namespace tests {

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


// Test a simple range parse.
TEST(ValuesTest, RangesParse)
{
  Value::Ranges ranges;

  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(10);

  Value::Ranges parsed =
    parse("[1-10]").get().ranges();

  EXPECT_EQ(ranges, parsed);
}


// Unit test coalescing given ranges.
TEST(ValuesTest, RangesCoalesce)
{
  // Multiple overlap with explicit coalesce.
  // Explicitly construct [1-4, 3-5, 7-8, 8-10].
  Value::Ranges ranges;
  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(4);
  range = ranges.add_range();
  range->set_begin(3);
  range->set_end(5);
  range = ranges.add_range();
  range->set_begin(7);
  range->set_end(8);
  range = ranges.add_range();
  range->set_begin(8);
  range->set_end(10);

  mesos::coalesce(&ranges);

  // Should be [1-5, 7-10].
  ASSERT_EQ(2, ranges.range_size());
  EXPECT_EQ(1, ranges.range(0).begin());
  EXPECT_EQ(5, ranges.range(0).end());
  EXPECT_EQ(7, ranges.range(1).begin());
  EXPECT_EQ(10, ranges.range(1).end());
}


// Test coalescing given ranges via parse.
// Note: As the == operator triggers coalesce as well, we should
// check ranges_size() explicitly.
TEST(ValuesTest, RangesCoalesceParse)
{
  // Test multiple ranges against explicitly constructed.
  Value::Ranges ranges;

  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(10);
  Value::Ranges parsed =
    parse("[4-6, 6-8, 5-9, 3-4, 2-5, 4-6, 1-1, 10-10, 4-6]").get().ranges();

  EXPECT_EQ(ranges, parsed);
  EXPECT_EQ(1, parsed.range_size());

  // Simple overlap.
  parsed = parse("[4-6, 6-8]").get().ranges();
  Value::Ranges expected = parse("[4-8]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Simple overlap unordered.
  parsed = parse("[6-8, 4-6]").get().ranges();
  expected = parse("[4-8]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Subsumed range.
  parsed = parse("[1-10, 8-10]").get().ranges();
  expected = parse("[1-10]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Completely subsumed.
  parsed = parse("[1-11, 8-10]").get().ranges();
  expected = parse("[1-11]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple overlapping ranges.
  parsed = parse("[1-4, 4-5, 7-8, 8-10]").get().ranges();
  expected = parse("[1-5, 7-10]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Multiple overlap mixed.
  parsed = parse("[7-8, 1-4, [8-10], 4-5]").get().ranges();
  expected = parse("[1-5, 7-10]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Simple neighboring with overlap.
  parsed = parse("[4-6, 7-8]").get().ranges();
  expected = parse("[4-8]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Single neighbouring.
  parsed = parse("[4-6, 7-7]").get().ranges();
  expected = parse("[4-7]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple ranges coalescing into a single one.
  parsed = parse("[4-6, 7-7, 8-8, 9-9]").get().ranges();
  expected = parse("[4-9]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple ranges coalescing into multiple ranges.
  parsed = parse("[4-6, 7-7, 9-10, 9-11]").get().ranges();
  expected = parse("[4-7, 9-11]").get().ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Multiple duplicates.
  parsed = parse("[6-8, 6-8, 4-6, 6-8]").get().ranges();
  expected = parse("[4-8]").get().ranges();
  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());
}


// Test adding a new range to an existing coalesced range.
// Note: As the == operator triggers coalesce as well, we should
// check ranges_size() explicitly.
TEST(ValuesTest, AddRangeCoalesce)
{
  // Multiple overlap with explicit coalesce.
  // Explicitly construct [1-4, 8-10].
  Value::Ranges ranges;
  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(4);
  range = ranges.add_range();
  range->set_begin(8);
  range->set_end(10);

  // Range [4-8].
  Value::Range range2;
  range2.set_begin(4);
  range2.set_end(8);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10].
  ASSERT_EQ(1, ranges.range_size());
  EXPECT_EQ(1, ranges.range(0).begin());
  EXPECT_EQ(10, ranges.range(0).end());

  // Multiple neighboring with explicit coalesce.
  // Range [5-7].
  range2.set_begin(5);
  range2.set_end(7);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10].
  ASSERT_EQ(1, ranges.range_size());
  EXPECT_EQ(1, ranges.range(0).begin());
  EXPECT_EQ(10, ranges.range(0).end());

  // Completely subsumed.
  // Range [5-7] (as before).
  range2.set_begin(5);
  range2.set_end(7);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10].
  ASSERT_EQ(1, ranges.range_size());
  EXPECT_EQ(1, ranges.range(0).begin());
  EXPECT_EQ(10, ranges.range(0).end());

  // None overlapping.
  // Range [5-7] (as before).
  range2.set_begin(20);
  range2.set_end(21);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10, 20-21].
  ASSERT_EQ(2, ranges.range_size());
  EXPECT_EQ(1, ranges.range(0).begin());
  EXPECT_EQ(10, ranges.range(0).end());
  EXPECT_EQ(20, ranges.range(1).begin());
  EXPECT_EQ(21, ranges.range(1).end());
}

// Test adding two ranges.
TEST(ValuesTest, RangesAddition)
{
  // Overlaps on right.
  Value::Ranges ranges1 = parse("[3-8]").get().ranges();
  Value::Ranges ranges2 = parse("[4-10]").get().ranges();

  EXPECT_EQ(parse("[3-10]").get().ranges(), ranges1 + ranges2);

  // Overlapps on right.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[1-4]").get().ranges();

  EXPECT_EQ(parse("[1-8]").get().ranges(), ranges1 + ranges2);

  // Completely subsumed.
  ranges1 = parse("[2-3]").get().ranges();
  ranges2 = parse("[1-4]").get().ranges();

  EXPECT_EQ(parse("[1-4]").get().ranges(), ranges1 + ranges2);

  // Neighbouring right.
  ranges1 = parse("[2-3]").get().ranges();
  ranges2 = parse("[4-6]").get().ranges();

  EXPECT_EQ(parse("[2-6]").get().ranges(), ranges1 + ranges2);

  // Neighbouring left.
  ranges1 = parse("[3-5]").get().ranges();
  ranges2 = parse("[1-2]").get().ranges();

  EXPECT_EQ(parse("[1-5]").get().ranges(), ranges1 + ranges2);

  // Fills gap.
  ranges1 = parse("[3-5, 7-8]").get().ranges();
  ranges2 = parse("[6-6]").get().ranges();

  EXPECT_EQ(parse("[3-8]").get().ranges(), ranges1 + ranges2);

  // Fills double gap.
  ranges1 = parse("[1-4, 9-10, 20-22, 26-30]").get().ranges();
  ranges2 = parse("[5-8, 23-25]").get().ranges();

  EXPECT_EQ(parse("[1-10, 20-30]").get().ranges(), ranges1 + ranges2);
}

// Test subtracting two ranges.
TEST(ValuesTest, RangesSubtraction)
{
  // Completely subsumes.
  Value::Ranges ranges1 = parse("[3-8]").get().ranges();
  Value::Ranges ranges2 = parse("[1-10]").get().ranges();

  EXPECT_EQ(parse("[]").get().ranges(), ranges1 - ranges2);

  // Subsummed on left.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[3-5]").get().ranges();

  EXPECT_EQ(parse("[6-8]").get().ranges(), ranges1 - ranges2);

  // Subsummed on right.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[5-8]").get().ranges();

  EXPECT_EQ(parse("[3-4]").get().ranges(), ranges1 - ranges2);

  // Subsummed in the middle.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[5-6]").get().ranges();

  EXPECT_EQ(parse("[3-4, 7-8]").get().ranges(), ranges1 - ranges2);

  // Overlaps to the left.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[1-3]").get().ranges();

  EXPECT_EQ(parse("[4-8]").get().ranges(), ranges1 - ranges2);

  // Overlaps to the right.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[5-10]").get().ranges();

  EXPECT_EQ(parse("[3-4]").get().ranges(), ranges1 - ranges2);

  // Doesn't overlap right.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[9-10]").get().ranges();

  EXPECT_EQ(parse("[3-8]").get().ranges(), ranges1 - ranges2);

  // Doesn't overlap left.
  ranges1 = parse("[3-8]").get().ranges();
  ranges2 = parse("[1-2]").get().ranges();

  EXPECT_EQ(parse("[3-8]").get().ranges(), ranges1 - ranges2);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
