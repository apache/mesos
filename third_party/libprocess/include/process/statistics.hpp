#ifndef __PROCESS_STATISTICS_HPP__
#define __PROCESS_STATISTICS_HPP__

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>

namespace process {

// Forward declarations.
class StatisticsProcess;

// Provides an in-memory time series of statistics over some window
// (values are truncated outside of the window, but no limit is
// currently placed on the number of values within a window).
class Statistics
{
public:
  Statistics(const Seconds& window);
  ~Statistics();

  // Returns the time series of a statistic.
  process::Future<std::map<Seconds, double> > get(
      const std::string& name,
      const Option<Seconds>& start = Option<Seconds>::none(),
      const Option<Seconds>& stop = Option<Seconds>::none());

  // Sets the current value of a statistic.
  void set(const std::string& name, double value);

  // Increments the current value of a statistic. If no statistic was
  // previously present, an initial value of 0.0 is used.
  void increment(const std::string& name);

  // Decrements the current value of a statistic. If no statistic was
  // previously present, an initial value of 0.0 is used.
  void decrement(const std::string& name);

private:
  StatisticsProcess* process;
};

} // namespace process {

#endif // __PROCESS_STATISTICS_HPP__
