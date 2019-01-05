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

#include <stdint.h>

#include <sstream>

#include <gtest/gtest.h>

#include <mesos/values.hpp>

#include <stout/gtest.hpp>
#include <stout/interval.hpp>
#include <stout/try.hpp>

#include "common/values.hpp"

#include "master/master.hpp"

using namespace mesos::internal::values;

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
  ASSERT_EQ(Value::SCALAR, result1->type());
  EXPECT_DOUBLE_EQ(45.55, result1->scalar().value());

  // Test parsing ranges type.
  Try<Value> result2 = parse("[10000-20000, 30000-50000]");
  ASSERT_SOME(result2);
  ASSERT_EQ(Value::RANGES, result2->type());
  EXPECT_EQ(2, result2->ranges().range_size());
  EXPECT_EQ(10000u, result2->ranges().range(0).begin());
  EXPECT_EQ(20000u, result2->ranges().range(0).end());
  EXPECT_EQ(30000u, result2->ranges().range(1).begin());
  EXPECT_EQ(50000u, result2->ranges().range(1).end());

  // Test parsing set type.
  Try<Value> result3 = parse("{sda1, sda2}");
  ASSERT_SOME(result3);
  ASSERT_EQ(Value::SET, result3->type());
  ASSERT_EQ(2, result3->set().item_size());
  EXPECT_EQ("sda1", result3->set().item(0));
  EXPECT_EQ("sda2", result3->set().item(1));

  // Test parsing text type.
  Try<Value> result4 = parse("123abc,s");
  ASSERT_SOME(result4);
  ASSERT_EQ(Value::TEXT, result4->type());
  ASSERT_EQ("123abc,s", result4->text().value());
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

  EXPECT_ERROR(parse("nan"));
  EXPECT_ERROR(parse("-nan"));

  EXPECT_ERROR(parse("inf"));
  EXPECT_ERROR(parse("-inf"));

  EXPECT_ERROR(parse("infinity"));
  EXPECT_ERROR(parse("-infinity"));
}


TEST(ValuesTest, SetSubtraction)
{
  Value::Set set1 = parse("{sda1, sda2, sda3}")->set();
  Value::Set set2 = parse("{sda2, sda3}")->set();
  Value::Set set3 = parse("{sda4}")->set();

  set1 -= set2;

  EXPECT_EQ(set1, parse("{sda1}")->set());

  set3 -= set1;

  EXPECT_EQ(set3, parse("{sda4}")->set());
}


// Test a simple range parse.
TEST(ValuesTest, RangesParse)
{
  Value::Ranges ranges;

  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(10);

  Value::Ranges parsed =
    parse("[1-10]")->ranges();

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
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(5U, ranges.range(0).end());
  EXPECT_EQ(7U, ranges.range(1).begin());
  EXPECT_EQ(10U, ranges.range(1).end());
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
    parse("[4-6, 6-8, 5-9, 3-4, 2-5, 4-6, 1-1, 10-10, 4-6]")->ranges();

  EXPECT_EQ(ranges, parsed);
  EXPECT_EQ(1, parsed.range_size());

  // Simple overlap.
  parsed = parse("[4-6, 6-8]")->ranges();
  Value::Ranges expected = parse("[4-8]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Simple overlap unordered.
  parsed = parse("[6-8, 4-6]")->ranges();
  expected = parse("[4-8]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Subsumed range.
  parsed = parse("[1-10, 8-10]")->ranges();
  expected = parse("[1-10]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Completely subsumed.
  parsed = parse("[1-11, 8-10]")->ranges();
  expected = parse("[1-11]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple overlapping ranges.
  parsed = parse("[1-4, 4-5, 7-8, 8-10]")->ranges();
  expected = parse("[1-5, 7-10]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Multiple overlap mixed.
  parsed = parse("[7-8, 1-4, [8-10], 4-5]")->ranges();
  expected = parse("[1-5, 7-10]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Simple neighboring with overlap.
  parsed = parse("[4-6, 7-8]")->ranges();
  expected = parse("[4-8]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Single neighbouring.
  parsed = parse("[4-6, 7-7]")->ranges();
  expected = parse("[4-7]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple ranges coalescing into a single one.
  parsed = parse("[4-6, 7-7, 8-8, 9-9]")->ranges();
  expected = parse("[4-9]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(1, parsed.range_size());

  // Multiple ranges coalescing into multiple ranges.
  parsed = parse("[4-6, 7-7, 9-10, 9-11]")->ranges();
  expected = parse("[4-7, 9-11]")->ranges();

  EXPECT_EQ(parsed, expected);
  EXPECT_EQ(2, parsed.range_size());

  // Multiple duplicates.
  parsed = parse("[6-8, 6-8, 4-6, 6-8]")->ranges();
  expected = parse("[4-8]")->ranges();
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
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(10U, ranges.range(0).end());

  // Multiple neighboring with explicit coalesce.
  // Range [5-7].
  range2.set_begin(5);
  range2.set_end(7);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10].
  ASSERT_EQ(1, ranges.range_size());
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(10U, ranges.range(0).end());

  // Completely subsumed.
  // Range [5-7] (as before).
  range2.set_begin(5);
  range2.set_end(7);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10].
  ASSERT_EQ(1, ranges.range_size());
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(10U, ranges.range(0).end());

  // None overlapping.
  // Range [5-7] (as before).
  range2.set_begin(20);
  range2.set_end(21);

  mesos::coalesce(&ranges, range2);

  // Should be coalesced to [1-10, 20-21].
  ASSERT_EQ(2, ranges.range_size());
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(10U, ranges.range(0).end());
  EXPECT_EQ(20U, ranges.range(1).begin());
  EXPECT_EQ(21U, ranges.range(1).end());
}


// Test converting Ranges to IntervalSet.
TEST(ValuesTest, RangesToIntervalSet)
{
  Value::Ranges ranges;
  IntervalSet<uint64_t> set;
  IntervalSet<uint64_t>::iterator interval;

  // Initialize Ranges value as [1-1, 3-5, 7-8].
  Value::Range* range = ranges.add_range();
  range->set_begin(1);
  range->set_end(1);
  range = ranges.add_range();
  range->set_begin(3);
  range->set_end(5);
  range = ranges.add_range();
  range->set_begin(7);
  range->set_end(8);

  // Convert Ranges value to IntervalSet value.
  set = rangesToIntervalSet<uint64_t>(ranges).get();

  // Verify converting result which should be {[1,2), [3-6), [7-9)}.
  ASSERT_EQ(3U, set.intervalCount());
  interval = set.begin();
  EXPECT_EQ(1U, interval->lower());
  EXPECT_EQ(2U, interval->upper());
  interval++;
  EXPECT_EQ(3U, interval->lower());
  EXPECT_EQ(6U, interval->upper());
  interval++;
  EXPECT_EQ(7U, interval->lower());
  EXPECT_EQ(9U, interval->upper());
  interval++;
  EXPECT_EQ(set.end(), interval);
}


// Test converting IntervalSet to Ranges.
TEST(ValuesTest, IntervalSetToRanges)
{
  Value::Ranges ranges;
  IntervalSet<uint64_t> set;

  // Initialize IntervalSet value as {[1-1], [3-4], [7-9]}.
  set += (Bound<uint64_t>::closed(1), Bound<uint64_t>::closed(1));
  set += (Bound<uint64_t>::closed(3), Bound<uint64_t>::closed(4));
  set += (Bound<uint64_t>::closed(7), Bound<uint64_t>::closed(9));

  // Convert IntervalSet value to Ranges value.
  ranges = intervalSetToRanges(set);

  // Verify converting result which should be [1-1, 3-4, 7-9].
  ASSERT_EQ(3, ranges.range_size());
  EXPECT_EQ(1U, ranges.range(0).begin());
  EXPECT_EQ(1U, ranges.range(0).end());
  EXPECT_EQ(3U, ranges.range(1).begin());
  EXPECT_EQ(4U, ranges.range(1).end());
  EXPECT_EQ(7U, ranges.range(2).begin());
  EXPECT_EQ(9U, ranges.range(2).end());
}


// Test adding two ranges.
TEST(ValuesTest, RangesAddition)
{
  // Overlaps on right.
  Value::Ranges ranges1 = parse("[3-8]")->ranges();
  Value::Ranges ranges2 = parse("[4-10]")->ranges();

  EXPECT_EQ(parse("[3-10]")->ranges(), ranges1 + ranges2);

  // Overlapps on right.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[1-4]")->ranges();

  EXPECT_EQ(parse("[1-8]")->ranges(), ranges1 + ranges2);

  // Completely subsumed.
  ranges1 = parse("[2-3]")->ranges();
  ranges2 = parse("[1-4]")->ranges();

  EXPECT_EQ(parse("[1-4]")->ranges(), ranges1 + ranges2);

  // Neighbouring right.
  ranges1 = parse("[2-3]")->ranges();
  ranges2 = parse("[4-6]")->ranges();

  EXPECT_EQ(parse("[2-6]")->ranges(), ranges1 + ranges2);

  // Neighbouring left.
  ranges1 = parse("[3-5]")->ranges();
  ranges2 = parse("[1-2]")->ranges();

  EXPECT_EQ(parse("[1-5]")->ranges(), ranges1 + ranges2);

  // Fills gap.
  ranges1 = parse("[3-5, 7-8]")->ranges();
  ranges2 = parse("[6-6]")->ranges();

  EXPECT_EQ(parse("[3-8]")->ranges(), ranges1 + ranges2);

  // Fills double gap.
  ranges1 = parse("[1-4, 9-10, 20-22, 26-30]")->ranges();
  ranges2 = parse("[5-8, 23-25]")->ranges();

  EXPECT_EQ(parse("[1-10, 20-30]")->ranges(), ranges1 + ranges2);
}


// Test subtracting two ranges.
TEST(ValuesTest, RangesSubtraction)
{
  // Ranges1 is empty.
  Value::Ranges ranges1 = parse("[]")->ranges();
  Value::Ranges ranges2 = parse("[1-10]")->ranges();

  EXPECT_EQ(parse("[]")->ranges(), ranges1 - ranges2);

  // Ranges2 is empty.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[]")->ranges();

  EXPECT_EQ(parse("[3-8]")->ranges(), ranges1 - ranges2);

  // Completely subsumes.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[1-10]")->ranges();

  EXPECT_EQ(parse("[]")->ranges(), ranges1 - ranges2);

  // Subsummed on left.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[3-5]")->ranges();

  EXPECT_EQ(parse("[6-8]")->ranges(), ranges1 - ranges2);

  // Subsummed on right.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[5-8]")->ranges();

  EXPECT_EQ(parse("[3-4]")->ranges(), ranges1 - ranges2);

  // Subsummed in the middle.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[5-6]")->ranges();

  EXPECT_EQ(parse("[3-4, 7-8]")->ranges(), ranges1 - ranges2);

  // Overlaps to the left.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[1-3]")->ranges();

  EXPECT_EQ(parse("[4-8]")->ranges(), ranges1 - ranges2);

  // Overlaps to the right.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[5-10]")->ranges();

  EXPECT_EQ(parse("[3-4]")->ranges(), ranges1 - ranges2);

  // Doesn't overlap right.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[9-10]")->ranges();

  EXPECT_EQ(parse("[3-8]")->ranges(), ranges1 - ranges2);

  // Doesn't overlap left.
  ranges1 = parse("[3-8]")->ranges();
  ranges2 = parse("[1-2]")->ranges();

  EXPECT_EQ(parse("[3-8]")->ranges(), ranges1 - ranges2);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
