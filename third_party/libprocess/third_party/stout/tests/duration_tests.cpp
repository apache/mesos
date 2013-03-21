#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/try.hpp>


TEST(DurationTest, Test)
{
  Try<Duration> _3hrs = Duration::parse("3hrs");
  ASSERT_SOME(_3hrs);

  EXPECT_EQ(Hours(3.0), _3hrs.get());
  EXPECT_EQ(Minutes(180.0), _3hrs.get());
  EXPECT_EQ(Seconds(10800.0), _3hrs.get());
  EXPECT_EQ(Milliseconds(10800000.0), _3hrs.get());

  EXPECT_EQ(Milliseconds(1000.0), Seconds(1.0));

  EXPECT_GT(Weeks(1.0), Days(6.0));

  EXPECT_LT(Hours(23.0), Days(1.0));

  EXPECT_LE(Hours(24.0), Days(1.0));
  EXPECT_GE(Hours(24.0), Days(1.0));

  EXPECT_NE(Minutes(59.0), Hours(1.0));

  Try<Duration> _3_5hrs = Duration::parse("3.5hrs");
  ASSERT_SOME(_3_5hrs);

  EXPECT_EQ(Hours(3.5), _3_5hrs.get());
}
