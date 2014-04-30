#ifndef __PROCESS_METRICS_TIMER_HPP__
#define __PROCESS_METRICS_TIMER_HPP__

#include <string>

#include <process/future.hpp>
#include <process/internal.hpp>

#include <process/metrics/metric.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

namespace process {
namespace metrics {

// A Metric that represents a timed event in milliseconds.
// TODO(dhamon): Allow the user to choose the unit of duration.
// We could do this by adding methods on Duration subclasses to return
// the double value and unit string directly.
class Timer : public Metric
{
public:
  // The Timer name will have "_ms" as an implicit unit suffix.
  Timer(const std::string& name, const Option<Duration>& window = None())
    : Metric(name + "_ms", window),
      data(new Data()) {}

  Future<double> value() const
  {
    Future<double> value;

    process::internal::acquire(&data->lock);
    {
      if (data->lastValue.isSome()) {
        value = data->lastValue.get();
      } else {
        value = Failure("No value");
      }
    }
    process::internal::release(&data->lock);

    return value;
  }

  // Start the stopwatch.
  void start()
  {
    process::internal::acquire(&data->lock);
    {
      data->stopwatch.start();
    }
    process::internal::release(&data->lock);
  }

  // Stop the stopwatch.
  void stop()
  {
    double value;

    process::internal::acquire(&data->lock);
    {
      data->stopwatch.stop();

      // Assume milliseconds for now.
      data->lastValue = data->stopwatch.elapsed().ms();

      value = data->lastValue.get();
    }
    process::internal::release(&data->lock);

    push(value);
  }

  // Time an asynchronous event.
  template<typename T>
  Future<T> time(const Future<T>& future)
  {
    Stopwatch stopwatch;
    stopwatch.start();

    // We need to take a copy of 'this' here to ensure that the
    // Timer is not destroyed in the interim.
    future
      .onAny(lambda::bind(_time, stopwatch, *this));

    return future;
  }

private:
  struct Data {
    Data() : lock(0) {}

    int lock;
    Stopwatch stopwatch;
    Option<double> lastValue;
  };

  static void _time(Stopwatch stopwatch, Timer that)
  {
    stopwatch.stop();

    double value;

    process::internal::acquire(&that.data->lock);
    {
      // Assume milliseconds for now.
      that.data->lastValue = stopwatch.elapsed().ms();
      value = that.data->lastValue.get();
    }
    process::internal::release(&that.data->lock);

    that.push(value);
  }

  memory::shared_ptr<Data> data;
};

} // namespace metrics
} // namespace process

#endif // __PROCESS_METRICS_TIMER_HPP__
