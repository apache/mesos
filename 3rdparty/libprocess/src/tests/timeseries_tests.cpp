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

#include <list>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/timeseries.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>

using std::list;

using process::Clock;
using process::Time;
using process::TimeSeries;

list<int> toList(const TimeSeries<int>& series)
{
  list<int> result;
  foreach (const TimeSeries<int>::Value& value, series.get()) {
    result.push_back(value.data);
  }
  return result;
}


TEST(TimeSeriesTest, Set)
{
  TimeSeries<int> series;

  ASSERT_TRUE(series.empty());

  series.set(1);

  ASSERT_FALSE(series.empty());

  const Option<TimeSeries<int>::Value> latest = series.latest();

  ASSERT_SOME(latest);
  ASSERT_EQ(1, latest->data);
}


TEST(TimeSeriesTest, Sparsify)
{
  // We have to pause the clock because this test often results
  // in to set() operations occurring at the same time according
  // to Clock::now().
  Clock::pause();
  Time now = Clock::now();

  // Create a time series and fill it to its capacity.
  TimeSeries<int> series(Duration::max(), 10);

  series.set(0, now);
  series.set(1, now + Seconds(1));
  series.set(2, now + Seconds(2));
  series.set(3, now + Seconds(3));
  series.set(4, now + Seconds(4));
  series.set(5, now + Seconds(5));
  series.set(6, now + Seconds(6));
  series.set(7, now + Seconds(7));
  series.set(8, now + Seconds(8));
  series.set(9, now + Seconds(9));

  ASSERT_EQ(list<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), toList(series));

  // Verify the sparsification pattern.
  series.set(10, now + Seconds(10));
  ASSERT_EQ(list<int>({0, 2, 3, 4, 5, 6, 7, 8, 9, 10}), toList(series));

  series.set(11, now + Seconds(11));
  ASSERT_EQ(list<int>({0, 2, 4, 5, 6, 7, 8, 9, 10, 11}), toList(series));

  series.set(12, now + Seconds(12));
  ASSERT_EQ(list<int>({0, 2, 4, 6, 7, 8, 9, 10, 11, 12}), toList(series));

  series.set(13, now + Seconds(13));
  ASSERT_EQ(list<int>({0, 2, 4, 6, 8, 9, 10, 11, 12, 13}), toList(series));

  series.set(14, now + Seconds(14));
  ASSERT_EQ(list<int>({0, 2, 4, 6, 8, 10, 11, 12, 13, 14}), toList(series));

  // Now we expect a new round of sparsification to occur, starting
  // again from the beginning.
  series.set(15, now + Seconds(15));
  ASSERT_EQ(list<int>({0, 4, 6, 8, 10, 11, 12, 13, 14, 15}), toList(series));

  series.set(16, now + Seconds(16));
  ASSERT_EQ(list<int>({0, 4, 8, 10, 11, 12, 13, 14, 15, 16}), toList(series));

  series.set(17, now + Seconds(17));
  ASSERT_EQ(list<int>({0, 4, 8, 11, 12, 13, 14, 15, 16, 17}), toList(series));

  series.set(18, now + Seconds(18));
  ASSERT_EQ(list<int>({0, 4, 8, 11, 13, 14, 15, 16, 17, 18}), toList(series));

  series.set(19, now + Seconds(19));
  ASSERT_EQ(list<int>({0, 4, 8, 11, 13, 15, 16, 17, 18, 19}), toList(series));

  Clock::resume();
}


TEST(TimeSeriesTest, Truncate)
{
  // Test simple truncation first.
  Clock::pause();
  Time now = Clock::now();

  // Create a time series and fill it to its capacity.
  TimeSeries<int> series(Seconds(10), 10);

  series.set(0, now);
  series.set(1, now + Seconds(1));
  series.set(2, now + Seconds(2));
  series.set(3, now + Seconds(3));
  series.set(4, now + Seconds(4));
  series.set(5, now + Seconds(5));
  series.set(6, now + Seconds(6));
  series.set(7, now + Seconds(7));
  series.set(8, now + Seconds(8));
  series.set(9, now + Seconds(9));

  ASSERT_EQ(list<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), toList(series));

  // Cause the first 6 tasks to be truncated from the window.
  Clock::advance(Seconds(10 + 6));
  series.set(10, now + Seconds(10));

  ASSERT_EQ(list<int>({7, 8, 9, 10}), toList(series));

  Clock::resume();


  // Now test truncation in the face of sparsification.
  Clock::pause();
  now = Clock::now();

  series = TimeSeries<int>(Seconds(10), 10);

  series.set(0, now);
  series.set(1, now + Seconds(1));
  series.set(2, now + Seconds(2));
  series.set(3, now + Seconds(3));
  series.set(4, now + Seconds(4));
  series.set(5, now + Seconds(5));
  series.set(6, now + Seconds(6));
  series.set(7, now + Seconds(7));
  series.set(8, now + Seconds(8));
  series.set(9, now + Seconds(9));

  ASSERT_EQ(list<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), toList(series));

  // Move the sparsification candidate forward to ensure sparsification
  // is correct after a truncation occurs.
  series.set(10, now + Seconds(10));
  ASSERT_EQ(list<int>({0, 2, 3, 4, 5, 6, 7, 8, 9, 10}), toList(series));

  series.set(11, now + Seconds(11));
  ASSERT_EQ(list<int>({0, 2, 4, 5, 6, 7, 8, 9, 10, 11}), toList(series));

  series.set(12, now + Seconds(12));
  ASSERT_EQ(list<int>({0, 2, 4, 6, 7, 8, 9, 10, 11, 12}), toList(series));

  // Now the next sparsification candidate is '7'. First, we will
  // truncate exluding '7' and ensure sparsification proceeds as
  // expected.
  Clock::advance(Seconds(10 + 2));
  series.truncate();
  ASSERT_EQ(list<int>({4, 6, 7, 8, 9, 10, 11, 12}), toList(series));

  // Add 2 more items to return to capacity.
  series.set(13, now + Seconds(13));
  series.set(14, now + Seconds(14));
  ASSERT_EQ(list<int>({4, 6, 7, 8, 9, 10, 11, 12, 13, 14}), toList(series));

  // Now cause the time series to exceed capacity and ensure we
  // correctly remove '7'.
  series.set(15, now + Seconds(15));
  ASSERT_EQ(list<int>({4, 6, 8, 9, 10, 11, 12, 13, 14, 15}), toList(series));

  // Finally, let's truncate into the next sparsification candidate
  // '9', and ensure sparsification is reset.
  Clock::advance(Seconds(7)); // 2 + 7 = 9.
  series.truncate();
  ASSERT_EQ(list<int>({10, 11, 12, 13, 14, 15}), toList(series));

  // Get back to capacity and ensure sparsification starts from the
  // beginning.
  series.set(16, now + Seconds(16));
  series.set(17, now + Seconds(17));
  series.set(18, now + Seconds(18));
  series.set(19, now + Seconds(19));
  ASSERT_EQ(list<int>({10, 11, 12, 13, 14, 15, 16, 17, 18, 19}),
            toList(series));

  // Did we sparsify from the beginning?
  series.set(20, now + Seconds(20));
  ASSERT_EQ(list<int>({10, 12, 13, 14, 15, 16, 17, 18, 19, 20}),
            toList(series));

  // Done!
  Clock::resume();
}
