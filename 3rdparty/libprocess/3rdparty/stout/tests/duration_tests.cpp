#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


TEST(DurationTest, Comparison)
{
  EXPECT_EQ(Duration::zero(), Seconds(0));
  EXPECT_EQ(Minutes(180), Hours(3));
  EXPECT_EQ(Seconds(10800), Hours(3));
  EXPECT_EQ(Milliseconds(10800000), Hours(3));

  EXPECT_EQ(Milliseconds(1), Microseconds(1000));
  EXPECT_EQ(Milliseconds(1000), Seconds(1));

  EXPECT_GT(Weeks(1), Days(6));

  EXPECT_LT(Hours(23), Days(1));

  EXPECT_LE(Hours(24), Days(1));
  EXPECT_GE(Hours(24), Days(1));

  EXPECT_NE(Minutes(59), Hours(1));

  // Maintains precision for a 100 year duration.
  EXPECT_GT(Weeks(5217) + Nanoseconds(1), Weeks(5217));
  EXPECT_LT(Weeks(5217) - Nanoseconds(1), Weeks(5217));
}

TEST(DurationTest, ParseAndTry)
{
  EXPECT_SOME_EQ(Hours(3), Duration::parse("3hrs"));
  EXPECT_SOME_EQ(Hours(3) + Minutes(30), Duration::parse("3.5hrs"));

  EXPECT_SOME_EQ(Nanoseconds(3141592653), Duration::create(3.141592653));
  // Duration can hold only 9.22337e9 seconds.
  EXPECT_ERROR(Duration::create(10 * 1e9));
}

TEST(DurationTest, Arithmetic)
{
  Duration d = Seconds(11);
  d += Seconds(9);
  EXPECT_EQ(Seconds(20), d);

  d = Seconds(11);
  d -= Seconds(21);
  EXPECT_EQ(Seconds(-10), d);

  d = Seconds(10);
  d *= 2;
  EXPECT_EQ(Seconds(20), d);

  d = Seconds(10);
  d /= 2.5;
  EXPECT_EQ(Seconds(4), d);

  EXPECT_EQ(Seconds(20), Seconds(11) + Seconds(9));
  EXPECT_EQ(Seconds(-10), Seconds(11) - Seconds(21));
  EXPECT_EQ(Duration::create(3.3).get(), Seconds(10) * 0.33);
  EXPECT_EQ(Duration::create(1.25).get(), Seconds(10) / 8);

  EXPECT_EQ(Duration::create(Days(11).secs() + 9).get(), Days(11) + Seconds(9));
}


TEST(DurationTest, OutputFormat)
{
  // Truncated. Seconds in 15 digits of precision, max of double
  // type's precise digits.
  EXPECT_EQ("3.141592653secs",
            stringify(Duration::create(3.14159265358979).get()));
  EXPECT_EQ("3.14secs", stringify(Duration::create(3.14).get()));
  EXPECT_EQ("10hrs", stringify(Hours(10)));
  // This is the current expected way how it should be printed out.
  EXPECT_EQ("1.42857142857143weeks", stringify(Days(10)));
}
