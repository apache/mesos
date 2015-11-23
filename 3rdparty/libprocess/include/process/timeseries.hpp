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

#ifndef __PROCESS_TIMESERIES_HPP__
#define __PROCESS_TIMESERIES_HPP__

#include <algorithm> // For max.
#include <map>
#include <vector>

#include <process/clock.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

namespace process {

// Default statistic configuration variables.
// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in
// which these duration constants were not being initialized when
// having static linkage. This issue did not manifest in newer gcc's.
// Specifically, 4.2.1 was ok. So we've moved these to have external
// linkage but perhaps in the future we can revert this.
extern const Duration TIME_SERIES_WINDOW;
extern const size_t TIME_SERIES_CAPACITY;


// Provides an in-memory time series of statistics over some window.
// When the time series capacity is exceeded within the window, the
// granularity of older values is coarsened. This means, for
// high-frequency statistics that exceed the capacity, we keep a lot
// of recent data points (fine granularity), and keep fewer older
// data points (coarse granularity). The tunable bit here is the
// total number of data points to keep around, which informs how
// often to delete older data points, while still keeping a window
// worth of data.
// TODO(bmahler): Investigate using Google's btree implementation.
// This provides better insertion and lookup performance for large
// containers. This _should_ also provide significant memory
// savings. These are true because we have the following properties:
//   1. Our insertion order will mostly be in sorted order.
//   2. Our keys (Seconds) have efficient comparison operators.
// See: http://code.google.com/p/cpp-btree/
//      http://code.google.com/p/cpp-btree/wiki/UsageInstructions
template <typename T>
struct TimeSeries
{
  TimeSeries(const Duration& _window = TIME_SERIES_WINDOW,
             size_t _capacity = TIME_SERIES_CAPACITY)
    : window(_window),
      // The truncation technique requires at least 3 elements.
      capacity(std::max((size_t) 3, _capacity)) {}

  struct Value
  {
    Value(const Time& _time, const T& _data) : time(_time), data(_data) {}

    // Non-const for assignability.
    Time time;
    T data;
  };

  void set(const T& value, const Time& time = Clock::now())
  {
    // If we're not inserting at the end of the time series, then
    // we have to reset the sparsification index. Given that
    // out-of-order insertion is a rare use-case. This is a simple way
    // to keep insertions O(log(n)). No need to figure out how to
    // adjust the truncation index.
    if (!values.empty() && time < values.rbegin()->first) {
      index = None();
    }

    values[time] = value;
    truncate();
    sparsify();
  }

  // Returns the time series within the (optional) time range.
  std::vector<Value> get(
      const Option<Time>& start = None(),
      const Option<Time>& stop = None()) const
  {
    // Ignore invalid ranges.
    if (start.isSome() && stop.isSome() && start.get() > stop.get()) {
      return std::vector<Value>();
    }

    typename std::map<Time, T>::const_iterator lower = values.lower_bound(
        start.isSome() ? start.get() : Time::epoch());

    typename std::map<Time, T>::const_iterator upper = values.upper_bound(
        stop.isSome() ? stop.get() : Time::max());

    std::vector<Value> values;
    while (lower != upper) {
      values.push_back(Value(lower->first, lower->second));
      ++lower;
    }
    return values;
  }

  Option<Value> latest() const
  {
    if (empty()) {
      return None();
    }

    return Value(values.rbegin()->first, values.rbegin()->second);
  }

  bool empty() const { return values.empty(); }

  // Removes values outside the time window. This will ensure at
  // least one value remains. Note that this is called automatically
  // when writing to the time series, so this is only needed when
  // one wants to explicitly trigger a truncation.
  void truncate()
  {
    Time expired = Clock::now() - window;
    typename std::map<Time, T>::iterator upper_bound =
      values.upper_bound(expired);

    // Ensure at least 1 value remains.
    if (values.size() <= 1 || upper_bound == values.end()) {
      return;
    }

    // When truncating and there exists a next value considered
    // for sparsification, there are two cases to consider for
    // updating the index:
    //
    // Case 1: upper_bound < next
    //   ----------------------------------------------------------
    //       upper_bound index, next
    //                 v v
    //   Before: 0 1 2 3 4 5 6 7 ...
    //   ----------------------------------------------------------
    //                 next  index    After truncating, index is
    //                   v     v      must be adjusted:
    //   Truncate:     3 4 5 6 7 ...  index -= # elements removed
    //   ----------------------------------------------------------
    //              index, next
    //                   v
    //   After:        3 4 5 6 7 ...
    //   ----------------------------------------------------------
    //
    // Case 2: upper_bound >= next
    //   ----------------------------------------------------------
    //                   upper_bound, index, next
    //                   v
    //   Before: 0 1 2 3 4 5 6 7 ...
    //   ----------------------------------------------------------
    //                               After truncating, we must
    //   After:          4 5 6 7 ... reset index to None().
    //   ----------------------------------------------------------
    if (index.isSome() && upper_bound->first < next->first) {
      size_t size = values.size();
      values.erase(values.begin(), upper_bound);
      index = index.get() - (size - values.size());
    } else {
      index = None();
      values.erase(values.begin(), upper_bound);
    }
  }

private:
  // Performs "sparsification" to limit the size of the time series
  // to be within the capacity.
  //
  // The sparsifying technique is to iteratively halve the granularity
  // of the older half of the time series. Once sparsification reaches
  // the midpoint of the time series, it begins again from the
  // beginning.
  //
  // Sparsification results in the following granularity over time:
  // Initial: | ------------------------ A -------------------- |
  // Stage 1: | ------- 1/2 A ---------- | -------- B --------- |
  // Stage 2: | -- 1/4A --- | -- 1/2B -- | -------- C --------- |
  // Stage 3: | 1/8A | 1/4B | -- 1/2C -- | -------- D --------- |
  //     ...
  //
  // Each stage halves the size and granularity of time series prior
  // to sparsifying.
  void sparsify()
  {
    // We remove every other element up to the halfway point of the
    // time series, until we're within the capacity. If we reach the
    // half-way point of the time series, we'll start another
    // sparsification cycle from the beginning, for example:
    //
    // next             Time series with a capacity of 7.
    //   v              Initial state with 7 entries
    // 0 1 2 3 4 5 6
    //
    //   next           Insert '7'.
    //     v            Capacity is exceeded, we remove '1' and
    // 0 2 3 4 5 6 7    advance to remove '3' next.
    //
    //     next         Insert '8'.
    //       v          Capacity is exceeded, we remove '3' and
    // 0 2 4 5 6 7 8    advance to remove '5' next.
    //
    // next             Insert '9'.
    //   v              Capacity is exceeded, we remove '5' and now
    // 0 2 4 6 7 8 9    '7' is past the halfway mark, so we will reset
    //                  reset to the beginning and consider '2'.

    while (values.size() > capacity) {
      // If the index is uninitialized, or past the half-way point,
      // we set it back to the beginning.
      if (index.isNone() || index.get() > values.size() / 2) {
        // The second element is the initial deletion candidate.
        next = values.begin();
        ++next;
        index = 1;
      }

      next = values.erase(next);
      next++; // Skip one element.
      index = index.get() + 1;
    }
  }

  // Non-const for assignability.
  Duration window;
  size_t capacity;

  // We use a map instead of a hashmap to store the values because
  // that way we can retrieve a series in sorted order efficiently.
  std::map<Time, T> values;

  // Next deletion candidate. We store both the iterator and index.
  // The index is None initially, and whenever a value is appended
  // out-of-order. This means 'next' is only valid when 'index' is
  // Some.
  typename std::map<Time, T>::iterator next;
  Option<size_t> index;
};

} // namespace process {

#endif // __PROCESS_TIMESERIES_HPP__
