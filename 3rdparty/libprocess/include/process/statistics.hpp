#ifndef __PROCESS_STATISTICS_HPP__
#define __PROCESS_STATISTICS_HPP__

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/time.hpp>
#include <process/timeseries.hpp>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

namespace process {

// Forward declarations.
class Statistics;
class StatisticsProcess;

// Libprocess statistics handle.
// To be used from anywhere to manage statistics.
//
// Ex: process::statistics->increment("http", "num_requests");
//     process::statistics->set("http", "response_size", response.size());
//
// Statistics are exposed via JSON for external visibility.
extern Statistics* statistics;


// Default statistic configuration variables.
// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in
// which these duration constants were not being initialized when
// having static linkage. This issue did not manifest in newer gcc's.
// Specifically, 4.2.1 was ok. So we've moved these to have external
// linkage but perhaps in the future we can revert this.
extern const Duration STATISTICS_TRUNCATION_INTERVAL;


// Provides a collection of in-memory fixed capacity time series
// of statistics over some window. Values are truncated when they
// fall outside the window. "Sparsification" will occur when the
// capacity of a time series is exceeded inside the window.
class Statistics
{
public:
  Statistics(const Duration& window = TIME_SERIES_WINDOW,
             size_t capacity = TIME_SERIES_CAPACITY);
  ~Statistics();

  // Returns the time series of a statistic.
  process::Future<TimeSeries<double> > timeseries(
      const std::string& context,
      const std::string& name);

  // Sets the current value of a statistic at the current clock time
  // or at a specified time.
  void set(
      const std::string& context,
      const std::string& name,
      double value,
      const Time& time = Clock::now());

  // Increments the current value of a statistic. If no statistic was
  // previously present, an initial value of 0.0 is used.
  void increment(const std::string& context, const std::string& name);

  // Decrements the current value of a statistic. If no statistic was
  // previously present, an initial value of 0.0 is used.
  void decrement(const std::string& context, const std::string& name);

private:
  StatisticsProcess* process;
};

} // namespace process {

#endif // __PROCESS_STATISTICS_HPP__
