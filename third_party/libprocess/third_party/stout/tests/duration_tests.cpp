#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


TEST(DurationTest, Comparison)
{
  EXPECT_EQ(Minutes(180), Hours(3));
  EXPECT_EQ(Seconds(10800), Hours(3));
  EXPECT_EQ(Milliseconds(10800000), Hours(3));

  EXPECT_EQ(Milliseconds(1000), Seconds(1));

  EXPECT_GT(Weeks(1), Days(6));

  EXPECT_LT(Hours(23), Days(1));

  EXPECT_LE(Hours(24), Days(1));
  EXPECT_GE(Hours(24), Days(1));

  EXPECT_NE(Minutes(59), Hours(1));
}

TEST(DurationTest, Parse)
{
  EXPECT_SOME_EQ(Hours(3), Duration::parse("3hrs"));
  EXPECT_SOME_EQ(Hours(3.5), Duration::parse("3.5hrs"));
}

TEST(DurationTest, Arithmetic)
{
  Duration d = Seconds(11);
  d += Seconds(9);
  EXPECT_EQ(Seconds(20), d);

  d = Seconds(11);
  d -= Seconds(21);
  EXPECT_EQ(Seconds(-10), d);

  EXPECT_EQ(Seconds(20), Seconds(11) + Seconds(9));
  EXPECT_EQ(Seconds(-10), Seconds(11) - Seconds(21));

  EXPECT_EQ(Seconds(Days(11).secs() + 9), Days(11) + Seconds(9));
}


TEST(DurationTest, OutputFormat)
{
  EXPECT_EQ("3.141592653589793secs", stringify(Seconds(3.14159265358979323)));
  EXPECT_EQ("3.140000000000000secs", stringify(Seconds(3.14)));
}
