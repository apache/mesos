#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/statistics.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/list.hpp>

using namespace process;


List<double> toList(const TimeSeries<double>& series)
{
  List<double> result;
  foreach (const TimeSeries<double>::Value& value, series.get()) {
    result.push_back(value.data);
  }
  return result;
}


TEST(Statistics, set)
{
  Statistics statistics(Days(1));

  // Set one using Clock::now() implicitly.
  statistics.set("test", "statistic", 3.0);

  // Set one using Clock::now() explicitly.
  Time now = Clock::now();
  statistics.set("test", "statistic", 4.0, now);

  Future<TimeSeries<double> > values =
    statistics.timeseries("test", "statistic");

  AWAIT_ASSERT_READY(values);

  EXPECT_EQ(2, values.get().get().size());

  EXPECT_GE(Clock::now(), values.get().get().begin()->time);
  EXPECT_DOUBLE_EQ(3.0, values.get().get().begin()->data);

  EXPECT_EQ(List<double>(3.0, 4.0), toList(values.get()));
}


TEST(Statistics, increment)
{
  Statistics statistics;
  Future<TimeSeries<double> > values;

  statistics.increment("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(1.0), toList(values.get()));

  statistics.increment("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(1.0, 2.0), toList(values.get()));

  statistics.increment("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(1.0, 2.0, 3.0), toList(values.get()));
}


TEST(Statistics, decrement)
{
  Statistics statistics;
  Future<TimeSeries<double> > values;

  statistics.decrement("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(-1.0), toList(values.get()));

  statistics.decrement("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(-1.0, -2.0), toList(values.get()));

  statistics.decrement("test", "statistic");
  values = statistics.timeseries("test", "statistic");
  AWAIT_ASSERT_READY(values);
  EXPECT_EQ(List<double>(-1.0, -2.0, -3.0), toList(values.get()));
}
