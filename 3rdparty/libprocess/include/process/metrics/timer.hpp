#ifndef __PROCESS_METRICS_TIMER_HPP__
#define __PROCESS_METRICS_TIMER_HPP__

#include <string>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/internal.hpp>

#include <process/metrics/metric.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace process {
namespace metrics {

// A Metric that represents a timed event. It is templated on a Duration
// subclass that specifies the unit to use for the Timer.
// TODO(dhamon): Allow the user to choose the unit of duration.
// We could do this by adding methods on Duration subclasses to return
// the double value and unit string directly.
template<class T>
class Timer : public Metric
{
public:
  // The Timer name will have a unit suffix added automatically.
  Timer(const std::string& name, const Option<Duration>& window = None())
    : Metric(name + "_" + T::units(), window),
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

  // Start the Timer.
  void start()
  {
    process::internal::acquire(&data->lock);
    {
      data->start = Clock::now();
    }
    process::internal::release(&data->lock);
  }

  // Stop the Timer.
  void stop()
  {
    const Time stop = Clock::now();

    double value;

    process::internal::acquire(&data->lock);
    {
      data->lastValue = T(stop - data->start).value();

      value = data->lastValue.get();
    }
    process::internal::release(&data->lock);

    push(value);
  }

  // Time an asynchronous event.
  template<typename U>
  Future<U> time(const Future<U>& future)
  {
    // We need to take a copy of 'this' here to ensure that the
    // Timer is not destroyed in the interim.
    future
      .onAny(lambda::bind(_time, Clock::now(), *this));

    return future;
  }

private:
  struct Data {
    Data() : lock(0) {}

    int lock;
    Time start;
    Option<double> lastValue;
  };

  static void _time(Time start, Timer that)
  {
    const Time stop = Clock::now();

    double value;

    process::internal::acquire(&that.data->lock);
    {
      that.data->lastValue = T(stop - start).value();
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
