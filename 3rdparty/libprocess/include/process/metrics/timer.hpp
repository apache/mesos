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

#ifndef __PROCESS_METRICS_TIMER_HPP__
#define __PROCESS_METRICS_TIMER_HPP__

#include <atomic>
#include <memory>
#include <string>

#include <process/clock.hpp>
#include <process/future.hpp>

#include <process/metrics/metric.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>

namespace process {
namespace metrics {

// A Metric that represents a timed event. It is templated on a Duration
// subclass that specifies the unit to use for the Timer.
template <class T>
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

    synchronized (data->lock) {
      if (data->lastValue.isSome()) {
        value = data->lastValue.get();
      } else {
        value = Failure("No value");
      }
    }

    return value;
  }

  // Start the Timer.
  void start()
  {
    synchronized (data->lock) {
      data->start = Clock::now();
    }
  }

  // Stop the Timer.
  T stop()
  {
    const Time stop = Clock::now();

    T t(0);

    double value = 0.0;

    synchronized (data->lock) {
      t = T(stop - data->start);

      data->lastValue = t.value();

      value = data->lastValue.get();
    }

    push(value);

    return t;
  }

  // Time an asynchronous event.
  template <typename U>
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
    Data() = default;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Time start;
    Option<double> lastValue;
  };

  static void _time(Time start, Timer that)
  {
    const Time stop = Clock::now();

    double value;

    synchronized (that.data->lock) {
      that.data->lastValue = T(stop - start).value();
      value = that.data->lastValue.get();
    }

    that.push(value);
  }

  std::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_TIMER_HPP__
