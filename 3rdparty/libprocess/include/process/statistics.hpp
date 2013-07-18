#ifndef __PROCESS_STATISTICS_HPP__
#define __PROCESS_STATISTICS_HPP__

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/time.hpp>

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

const Duration STATISTICS_TRUNCATION_INTERVAL = Minutes(5);

// Provides an in-memory time series of statistics over some window
// (values are truncated outside of the window, but no limit is
// currently placed on the number of values within a window).
//
// TODO(bmahler): Time series granularity should be coarsened over
// time. This means, for high-frequency statistics, we keep a lot of
// recent data points (fine granularity), and keep fewer older data
// points (coarse granularity). The tunable bit here could be the
// total number of data points to keep around, which informs how
// often to delete older data points, while still keeping a window
// worth of data.
class Statistics
{
public:
  Statistics(const Duration& window);
  ~Statistics();

  // Returns the time series of a statistic.
  process::Future<std::map<Time, double> > timeseries(
      const std::string& context,
      const std::string& name,
      const Option<Time>& start = None(),
      const Option<Time>& stop = None());

  // Returns the latest value of a statistic.
  process::Future<Option<double> > get(
      const std::string& context,
      const std::string& name);

  // Returns the latest values of all statistics in the context.
  process::Future<std::map<std::string, double> > get(
      const std::string& context);

  // Sets the current value of a statistic at the current clock time
  // or at a specified time.
  void set(
      const std::string& context,
      const std::string& name,
      double value,
      const Time& time = Clock::now());

  // Archives the provided statistic time series.
  // This means two things:
  //   1. The statistic will no longer be part of the snapshot.
  //   2. However, the time series will be retained until the window
  //      expiration.
  void archive(const std::string& context, const std::string& name);

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
