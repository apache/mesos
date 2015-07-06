#include <set>

#include <gtest/gtest.h>

#include <stout/set.hpp>

TEST(SetTest, Set)
{
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1) | Set<int>(2));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) | Set<int>(1));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1) + 2);
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) + 2);
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2, 3) & Set<int>(1, 2));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) & Set<int>(1, 2));

  Set<int> left;
  left.insert(2);
  left.insert(4);

  Set<int> right;
  right.insert(1);
  right.insert(3);

  EXPECT_EQ(Set<int>(1, 2, 3, 4), left | right);
  EXPECT_EQ(Set<int>(), left & right);

  std::set<int> s = left;

  EXPECT_EQ(Set<int>(2, 4, 6), s + 6);
}
