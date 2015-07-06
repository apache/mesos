#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>

using namespace process;


TEST(StatisticsTest, Empty)
{
  TimeSeries<double> timeseries;

  EXPECT_NONE(Statistics<double>::from(timeseries));
}


TEST(StatisticsTest, Single)
{
  TimeSeries<double> timeseries;

  timeseries.set(0);

  EXPECT_NONE(Statistics<double>::from(timeseries));
}


TEST(StatisticsTest, Statistics)
{
  // Create a distribution of 10 values from -5 to 4.
  TimeSeries<double> timeseries;

  Time now = Clock::now();

  for (int i = -5; i <= 5; ++i) {
    now += Seconds(1);
    timeseries.set(i, now);
  }

  Option<Statistics<double> > statistics = Statistics<double>::from(timeseries);

  EXPECT_SOME(statistics);

  EXPECT_EQ(11u, statistics.get().count);

  EXPECT_FLOAT_EQ(-5.0, statistics.get().min);
  EXPECT_FLOAT_EQ(5.0, statistics.get().max);

  EXPECT_FLOAT_EQ(0.0, statistics.get().p50);
  EXPECT_FLOAT_EQ(4.0, statistics.get().p90);
  EXPECT_FLOAT_EQ(4.5, statistics.get().p95);
  EXPECT_FLOAT_EQ(4.9, statistics.get().p99);
  EXPECT_FLOAT_EQ(4.99, statistics.get().p999);
  EXPECT_FLOAT_EQ(4.999, statistics.get().p9999);
}
