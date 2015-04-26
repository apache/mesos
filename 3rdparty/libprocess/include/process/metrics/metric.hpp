#ifndef __PROCESS_METRICS_METRIC_HPP__
#define __PROCESS_METRICS_METRIC_HPP__

#include <memory>
#include <string>

#include <process/future.hpp>
#include <process/internal.hpp>
#include <process/owned.hpp>
#include <process/statistics.hpp>
#include <process/timeseries.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>

namespace process {
namespace metrics {

// The base class for Metrics such as Counter and Gauge.
class Metric {
public:
  virtual ~Metric() {}

  virtual Future<double> value() const = 0;

  const std::string& name() const
  {
    return data->name;
  }

  Option<Statistics<double>> statistics() const
  {
    Option<Statistics<double>> statistics = None();

    if (data->history.isSome()) {
      internal::acquire(&data->lock);
      {
        statistics = Statistics<double>::from(*data->history.get());
      }
      internal::release(&data->lock);
    }

    return statistics;
  }

protected:
  // Only derived classes can construct.
  Metric(const std::string& name, const Option<Duration>& window)
    : data(new Data(name, window)) {}

  // Inserts 'value' into the history for this metric.
  void push(double value) {
    if (data->history.isSome()) {
      Time now = Clock::now();

      internal::acquire(&data->lock);
      {
        data->history.get()->set(value, now);
      }
      internal::release(&data->lock);
    }
  }

private:
  struct Data {
    Data(const std::string& _name, const Option<Duration>& window)
      : name(_name),
        lock(0),
        history(None())
    {
      if (window.isSome()) {
        history =
          Owned<TimeSeries<double>>(new TimeSeries<double>(window.get()));
      }
    }

    const std::string name;

    int lock;

    Option<Owned<TimeSeries<double>>> history;
  };

  std::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_METRIC_HPP__
