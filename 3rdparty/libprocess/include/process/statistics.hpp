#ifndef __PROCESS_STATISTICS_HPP__
#define __PROCESS_STATISTICS_HPP__

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/owned.hpp>

namespace process {

// Forward declarations.
class Statistics;
class StatisticsProcess;

namespace meters {
  class Meter;
  class TimeRate;
}


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

  // Adds a meter for the statistic with the provided context and name.
  //   get(context, meter->name) will return the metered time series.
  // Returns an error if:
  //   -meter->name == name, or
  //   -The meter already exists.
  Future<Try<Nothing> > meter(
      const std::string& context,
      const std::string& name,
      Owned<meters::Meter> meter);

  // Sets the current value of a statistic at the current clock time
  // or at a specified time.
  void set(
      const std::string& context,
      const std::string& name,
      double value,
      const Time& time = Clock::now());

  // Archives the provided statistic time series, and any meters associated
  // with it. This means three things:
  //   1. The statistic will no longer be part of the snapshot.
  //   2. However, the time series will be retained until the window expiration.
  //   3. All meters associated with this statistic will be removed, both
  //      (1) and (2) will apply to the metered time series as well.
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


namespace meters {

// This is the interface for statistical meters.
// Meters provide additional metering on top of the raw statistical
// value. Ex: Track the maximum, average, rate, etc.
class Meter
{
protected:
  Meter(const std::string& _name) : name(_name) {}

public:
  virtual ~Meter() {}

  // Updates the meter with another input value.
  // Returns the new metered value, or none if no metered value can be produced.
  virtual Option<double> update(const Time& time, double value) = 0;

  const std::string name;
};


// Tracks the percent of time 'used' since the last update.
// Input values to this meter must be in seconds.
class TimeRate : public Meter
{
public:
  TimeRate(const std::string& name)
    : Meter(name), time(None()), value(0) {}

  virtual ~TimeRate() {}

  virtual Option<double> update(const Time& _time, double _value)
  {
    Option<double> rate;
    if (time.isSome()) {
      rate = (_value - value) / (_time - time.get()).secs();
    }

    time = _time;
    value = _value;
    return rate;
  }

private:
  Option<Time> time;
  double value;
};

} // namespace meters {
} // namespace process {

#endif // __PROCESS_STATISTICS_HPP__
