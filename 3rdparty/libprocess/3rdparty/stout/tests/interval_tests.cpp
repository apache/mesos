#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/interval.hpp>


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


TEST(IntervalTest, InvalidInterval)
{
  Interval<int> i1 = (Bound<int>::closed(1), Bound<int>::open(0));

  EXPECT_EQ(1, i1.lower());
  EXPECT_EQ(0, i1.upper());

  Interval<int> i2 = (Bound<int>::open(1), Bound<int>::closed(0));

  EXPECT_EQ(2, i2.lower());
  EXPECT_EQ(1, i2.upper());

  IntervalSet<int> set;

  set += i1;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());

  set += i2;

  EXPECT_TRUE(set.empty());
  EXPECT_EQ(0u, set.intervalCount());
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
