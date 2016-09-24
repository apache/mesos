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

#include <process/clock.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>

using process::Clock;
using process::Statistics;
using process::Time;
using process::TimeSeries;

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

  Option<Statistics<double>> statistics = Statistics<double>::from(timeseries);

  EXPECT_SOME(statistics);

  EXPECT_EQ(11u, statistics.get().count);

  EXPECT_DOUBLE_EQ(-5.0, statistics.get().min);
  EXPECT_DOUBLE_EQ(5.0, statistics.get().max);

  EXPECT_DOUBLE_EQ(0.0, statistics.get().p50);
  EXPECT_DOUBLE_EQ(4.0, statistics.get().p90);
  EXPECT_DOUBLE_EQ(4.5, statistics.get().p95);
  EXPECT_DOUBLE_EQ(4.9, statistics.get().p99);
  EXPECT_DOUBLE_EQ(4.99, statistics.get().p999);
  EXPECT_DOUBLE_EQ(4.999, statistics.get().p9999);
}
