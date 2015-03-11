#include <gtest/gtest.h>

#include <stout/bits.hpp>

TEST(BitsTest, CountSetBits)
{
  EXPECT_EQ(0, bits::countSetBits(0));
  EXPECT_EQ(6, bits::countSetBits(0xf3));
  EXPECT_EQ(15, bits::countSetBits(0xffbf));
  EXPECT_EQ(22, bits::countSetBits(0xfffffc));
  EXPECT_EQ(26, bits::countSetBits(0xfffffcf));
  EXPECT_EQ(32, bits::countSetBits(0xffffffff));
}
