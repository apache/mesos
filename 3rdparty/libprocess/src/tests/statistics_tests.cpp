#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>

using namespace process;


TEST(Statistics, empty)
{
  TimeSeries<double> timeseries;

  EXPECT_NONE(Statistics<double>::from(timeseries));
}


TEST(Statistics, statistics)
{
  // Create a distribution of 10 values from -5 to 4.
  TimeSeries<double> timeseries;

  Time now = Clock::now();
  timeseries.set(0.0, now);

  for (int i = -5; i < 5; ++i) {
    now += Seconds(1);
    timeseries.set(i, now);
  }

  Option<Statistics<double> > statistics = Statistics<double>::from(timeseries);

  EXPECT_SOME(statistics);

  EXPECT_FLOAT_EQ(-5.0, statistics.get().min);
  EXPECT_FLOAT_EQ(4.0, statistics.get().max);

  EXPECT_FLOAT_EQ(0.0, statistics.get().p50);
  EXPECT_FLOAT_EQ(3.0, statistics.get().p90);
  EXPECT_FLOAT_EQ(3.5, statistics.get().p95);
  EXPECT_FLOAT_EQ(3.9, statistics.get().p99);
  EXPECT_FLOAT_EQ(3.99, statistics.get().p999);
  EXPECT_FLOAT_EQ(3.999, statistics.get().p9999);
}
