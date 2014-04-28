#ifndef __PROCESS_STATISTICS_HPP__
#define __PROCESS_STATISTICS_HPP__

#include <glog/logging.h>

#include <algorithm>
#include <vector>

#include <process/timeseries.hpp>

#include <stout/foreach.hpp>
#include <stout/option.hpp>

namespace process {

// Represents statistics for a TimeSeries of data.
template <typename T>
struct Statistics
{
  // Returns Statistics for the given TimeSeries, or None() if the
  // TimeSeries is empty.
  // TODO(dhamon): Consider adding a histogram abstraction for better
  // performance.
  static Option<Statistics<T> > from(const TimeSeries<T>& timeseries)
  {
    std::vector<typename TimeSeries<T>::Value> values_ = timeseries.get();

    // We need at least 2 values to compute aggregates.
    if (values_.size() < 2) {
      return None();
    }

    std::vector<T> values;
    values.reserve(values_.size());

    foreach (const typename TimeSeries<T>::Value& value, values_) {
      values.push_back(value.data);
    }

    std::sort(values.begin(), values.end());

    Statistics statistics;

    statistics.count = values.size();

    statistics.min = values.front();
    statistics.max = values.back();

    statistics.p50 = percentile(values, 0.5);
    statistics.p90 = percentile(values, 0.90);
    statistics.p95 = percentile(values, 0.95);
    statistics.p99 = percentile(values, 0.99);
    statistics.p999 = percentile(values, 0.999);
    statistics.p9999 = percentile(values, 0.9999);

    return statistics;
  }

  size_t count;

  T min;
  T max;

  // TODO(dhamon): Consider making the percentiles we store dynamic.
  T p50;
  T p90;
  T p95;
  T p99;
  T p999;
  T p9999;

private:
  // Returns the requested percentile from the sorted values.
  // Note that we need at least two values to compute percentiles!
  // TODO(dhamon): Use a 'Percentage' abstraction.
  static T percentile(const std::vector<T>& values, double percentile)
  {
    CHECK_GE(values.size(), 2u);

    if (percentile <= 0.0) {
      return values.front();
    }

    if (percentile >= 1.0) {
      return values.back();
    }

    // Use linear interpolation.
    const double position = percentile * (values.size() - 1);
    const size_t index = static_cast<size_t>(floor(position));
    const double delta = position - index;

    CHECK_GE(index, 0u);
    CHECK_LT(index, values.size() - 1);

    return values[index] + delta * (values[index + 1] - values[index]);
  }
};

} // namespace process {

#endif // __PROCESS_STATISTICS_HPP__
