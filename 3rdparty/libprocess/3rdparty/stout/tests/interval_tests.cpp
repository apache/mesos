// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/interval.hpp>
#include <stout/stringify.hpp>


TEST(IntervalTest, Interval)
{
  Interval<int> i1 = (Bound<int>::open(1), Bound<int>::closed(3));

  EXPECT_EQ(2, i1.lower());
  EXPECT_EQ(4, i1.upper());

  Interval<int> i2 = (Bound<int>::open(3), Bound<int>::open(5));

  EXPECT_EQ(4, i2.lower());
  EXPECT_EQ(5, i2.upper());

  Interval<int> i3 = (Bound<int>::closed(5), Bound<int>::open(6));

  EXPECT_EQ(5, i3.lower());
  EXPECT_EQ(6, i3.upper());

  Interval<int> i4 = (Bound<int>::closed(6), Bound<int>::closed(7));

  EXPECT_EQ(6, i4.lower());
  EXPECT_EQ(8, i4.upper());

  IntervalSet<int> set;

  set += i1;

  EXPECT_FALSE(set.contains(1));
  EXPECT_TRUE(set.contains(2));
  EXPECT_TRUE(set.contains(3));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(2u, set.size());

  set += i2;

  EXPECT_TRUE(set.contains(4));
  EXPECT_FALSE(set.contains(5));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(3u, set.size());

  set += i3;

  EXPECT_TRUE(set.contains(5));
  EXPECT_FALSE(set.contains(6));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(4u, set.size());

  set += i4;

  EXPECT_TRUE(set.contains(6));
  EXPECT_TRUE(set.contains(7));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(6u, set.size());
}


TEST(IntervalTest, EmptyInterval)
{
  Interval<int> i1 = (Bound<int>::closed(1), Bound<int>::open(0));

  EXPECT_EQ(1, i1.lower());
  EXPECT_EQ(0, i1.upper());

  Interval<int> i2 = (Bound<int>::open(1), Bound<int>::closed(0));

  EXPECT_EQ(2, i2.lower());
  EXPECT_EQ(1, i2.upper());

  Interval<int> i3 = (Bound<int>::open(0), Bound<int>::open(0));

  EXPECT_EQ(1, i3.lower());
  EXPECT_EQ(0, i3.upper());

  Interval<int> i4 = (Bound<int>::closed(3), Bound<int>::closed(2));

  EXPECT_EQ(3, i4.lower());
  EXPECT_EQ(3, i4.upper());

  IntervalSet<int> set;

  set += i1;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());

  set += i2;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());

  set += i3;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());

  set += i4;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());

  set += (Bound<int>::closed(2), Bound<int>::closed(2));

  EXPECT_TRUE(set.contains(2));
  EXPECT_EQ(1u, set.size());
  EXPECT_EQ(1u, set.intervalCount());

  set += (Bound<int>::closed(0), Bound<int>::open(1));

  EXPECT_TRUE(set.contains(0));
  EXPECT_TRUE(set.contains(2));
  EXPECT_EQ(2u, set.size());
  EXPECT_EQ(2u, set.intervalCount());
}


TEST(IntervalTest, IntervalEqual)
{
  Interval<int> interval((Bound<int>::closed(1), Bound<int>::open(3)));
  Interval<int> interval2((Bound<int>::closed(1), Bound<int>::open(3)));
  Interval<int> interval3((Bound<int>::open(1), Bound<int>::open(1)));
  Interval<int> interval4((Bound<int>::open(1), Bound<int>::open(1)));
  Interval<int> interval5((Bound<int>::open(2), Bound<int>::closed(3)));

  EXPECT_EQ(interval, interval2);
  EXPECT_EQ(interval3, interval4);
  EXPECT_NE(interval, interval3);
  EXPECT_NE(interval2, interval5);
  EXPECT_NE(interval3, interval5);
}


TEST(IntervalTest, Constructor)
{
  IntervalSet<int> set(0);

  EXPECT_TRUE(set.contains(0));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(1u, set.size());

  IntervalSet<int> set2(Bound<int>::closed(1), Bound<int>::closed(2));

  EXPECT_TRUE(set2.contains(1));
  EXPECT_TRUE(set2.contains(2));
  EXPECT_EQ(1u, set2.intervalCount());
  EXPECT_EQ(2u, set2.size());
}


TEST(IntervalTest, Contains)
{
  IntervalSet<int> set;

  set += (Bound<int>::closed(1), Bound<int>::closed(10));

  EXPECT_TRUE(set.contains(1));
  EXPECT_TRUE(set.contains(10));
  EXPECT_FALSE(set.contains(11));
  EXPECT_FALSE(set.contains(0));

  EXPECT_TRUE(set.contains((Bound<int>::closed(2), Bound<int>::closed(10))));
  EXPECT_TRUE(set.contains((Bound<int>::closed(2), Bound<int>::open(11))));
  EXPECT_TRUE(set.contains((Bound<int>::open(2), Bound<int>::open(4))));
  EXPECT_FALSE(set.contains((Bound<int>::closed(5), Bound<int>::closed(11))));
  EXPECT_FALSE(set.contains((Bound<int>::open(0), Bound<int>::closed(20))));

  IntervalSet<int> set2;

  set2 += (Bound<int>::open(4), Bound<int>::open(10));

  EXPECT_TRUE(set.contains(set2));
  EXPECT_FALSE(set2.contains(set));

  IntervalSet<int> set3;

  EXPECT_TRUE(set.contains(set3));
  EXPECT_FALSE(set3.contains(set2));
}


TEST(IntervalTest, Addition)
{
  IntervalSet<int> set;

  set += 1;
  set += 2;
  set += 3;

  EXPECT_TRUE(set.contains(1));
  EXPECT_TRUE(set.contains(2));
  EXPECT_TRUE(set.contains(3));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(3u, set.size());

  set += (Bound<int>::closed(5), Bound<int>::closed(6));

  EXPECT_FALSE(set.contains(4));
  EXPECT_TRUE(set.contains(5));
  EXPECT_TRUE(set.contains(6));
  EXPECT_EQ(2u, set.intervalCount());
  EXPECT_EQ(5u, set.size());

  set += (Bound<int>::open(2), Bound<int>::open(5));

  EXPECT_TRUE(set.contains(4));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(6u, set.size());

  IntervalSet<int> set2;

  set2 += (Bound<int>::closed(8), Bound<int>::closed(9));

  set += set2;

  EXPECT_TRUE(set.contains(8));
  EXPECT_TRUE(set.contains(9));
  EXPECT_EQ(2u, set.intervalCount());
  EXPECT_EQ(8u, set.size());
}


TEST(IntervalTest, Subtraction)
{
  IntervalSet<int> set;

  set += (Bound<int>::closed(1), Bound<int>::closed(10));

  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(10u, set.size());

  set -= 5;

  EXPECT_FALSE(set.contains(5));
  EXPECT_EQ(2u, set.intervalCount());
  EXPECT_EQ(9u, set.size());

  set -= (Bound<int>::closed(2), Bound<int>::closed(8));

  EXPECT_FALSE(set.contains(2));
  EXPECT_FALSE(set.contains(8));
  EXPECT_EQ(2u, set.intervalCount());
  EXPECT_EQ(3u, set.size());

  set -= (Bound<int>::open(0), Bound<int>::open(2));

  EXPECT_FALSE(set.contains(1));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(2u, set.size());

  IntervalSet<int> set2;

  set2 += (Bound<int>::open(5), Bound<int>::closed(10));

  set -= set2;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());
}


TEST(IntervalTest, Intersection)
{
  IntervalSet<int> set;

  set += (Bound<int>::closed(1), Bound<int>::closed(3));

  EXPECT_TRUE(set.contains(1));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(3u, set.size());

  set &= (Bound<int>::open(1), Bound<int>::open(5));

  EXPECT_FALSE(set.contains(1));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(2u, set.size());

  IntervalSet<int> set2;

  set2 += 6;

  set &= set2;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());
}


TEST(IntervalTest, IntersectionTest)
{
  Interval<int> interval((Bound<int>::open(4), Bound<int>::closed(6)));
  Interval<int> interval2((Bound<int>::closed(1), Bound<int>::closed(5)));
  Interval<int> interval3((Bound<int>::closed(7), Bound<int>::closed(8)));

  IntervalSet<int> set((Bound<int>::closed(1), Bound<int>::closed(3)));
  IntervalSet<int> set2((Bound<int>::open(2), Bound<int>::open(4)));
  IntervalSet<int> set3((Bound<int>::open(6), Bound<int>::closed(7)));

  EXPECT_FALSE(set.intersects(interval));
  EXPECT_TRUE(set2.intersects(interval2));
  EXPECT_TRUE(set3.intersects(interval3));

  EXPECT_FALSE(set2.intersects(interval));
  EXPECT_TRUE(set2.intersects(interval2));
  EXPECT_FALSE(set2.intersects(interval3));

  EXPECT_TRUE(set.intersects(set2));
  EXPECT_TRUE(set2.intersects(set));
  EXPECT_FALSE(set3.intersects(set2));

  EXPECT_TRUE(interval.intersects(interval2));
  EXPECT_FALSE(interval2.intersects(interval3));
  EXPECT_FALSE(interval3.intersects(interval));
}


TEST(IntervalTest, LargeInterval)
{
  IntervalSet<int> set;

  set += (Bound<int>::open(1), Bound<int>::open(100));
  set += (Bound<int>::closed(100), Bound<int>::closed(1000000000));

  EXPECT_FALSE(set.contains(1));
  EXPECT_TRUE(set.contains(5));
  EXPECT_TRUE(set.contains(100));
  EXPECT_TRUE(set.contains(100000));
  EXPECT_TRUE(set.contains(1000000000));
  EXPECT_EQ(1u, set.intervalCount());
  EXPECT_EQ(999999999u, set.size());
}


TEST(IntervalTest, IntervalIteration)
{
  IntervalSet<int> set;

  set += (Bound<int>::closed(0), Bound<int>::closed(1));
  set += (Bound<int>::open(2), Bound<int>::open(4));
  set += (Bound<int>::closed(5), Bound<int>::open(7));
  set += (Bound<int>::open(7), Bound<int>::closed(9));

  EXPECT_EQ(4u, set.intervalCount());

  int index = 0;

  foreach (const Interval<int>& interval, set) {
    if (index == 0) {
      EXPECT_EQ(0, interval.lower());
      EXPECT_EQ(2, interval.upper());
    } else if (index == 1) {
      EXPECT_EQ(3, interval.lower());
      EXPECT_EQ(4, interval.upper());
    } else if (index == 2) {
      EXPECT_EQ(5, interval.lower());
      EXPECT_EQ(7, interval.upper());
    } else if (index == 3) {
      EXPECT_EQ(8, interval.lower());
      EXPECT_EQ(10, interval.upper());
    }
    index++;
  }
}


TEST(IntervalTest, Stream)
{
  EXPECT_EQ("[1,3)", stringify((Bound<int>::closed(1), Bound<int>::open(3))));
  EXPECT_EQ("[1,4)", stringify((Bound<int>::open(0), Bound<int>::closed(3))));
  EXPECT_EQ("[0,5)", stringify((Bound<int>::closed(0), Bound<int>::closed(4))));
  EXPECT_EQ("[2,3)", stringify((Bound<int>::open(1), Bound<int>::open(3))));
  EXPECT_EQ("[)", stringify((Bound<int>::closed(1), Bound<int>::open(1))));

  IntervalSet<int> set;

  set += (Bound<int>::open(7), Bound<int>::closed(9));
  EXPECT_EQ("{[8,10)}", stringify(set));

  set += 5;
  EXPECT_EQ("{[5,6)[8,10)}", stringify(set));

  set += (Bound<int>::closed(7), Bound<int>::closed(9));
  EXPECT_EQ("{[5,6)[7,10)}", stringify(set));
}


TEST(IntervalTest, InfixOperator)
{
  IntervalSet<int> set1(Bound<int>::closed(0), Bound<int>::closed(1));
  IntervalSet<int> set2(Bound<int>::closed(2), Bound<int>::open(3));
  IntervalSet<int> set3(Bound<int>::closed(0), Bound<int>::closed(2));

  EXPECT_EQ(set3, set1 + set2);
  EXPECT_EQ(set3, set1 + (Bound<int>::closed(2), Bound<int>::open(3)));
  EXPECT_EQ(set3, set1 + 2);

  EXPECT_EQ(set1, set3 - set2);
  EXPECT_EQ(set2, set3 - (Bound<int>::closed(0), Bound<int>::closed(1)));
  EXPECT_EQ(set1, set3 - 2);
}
