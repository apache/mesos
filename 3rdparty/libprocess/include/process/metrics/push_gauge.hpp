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

#ifndef __PROCESS_METRICS_PUSH_GAUGE_HPP__
#define __PROCESS_METRICS_PUSH_GAUGE_HPP__

#include <memory>
#include <string>

#include <process/metrics/metric.hpp>

namespace process {
namespace metrics {

// A Metric that represents an instantaneous measurement of a value
// (e.g. number of items in a queue).
//
// A push-based gauge differs from a pull-based gauge in that the
// client is responsible for pushing the latest value into the gauge
// whenever it changes. This can be challenging in some cases as it
// requires the client to have a good handle on when the gauge value
// changes (rather than just computing the current value when asked).
//
// NOTE: It is highly recommended to use push-based gauges if
// possible as they provide significant performance benefits over
// pull-based gauges. Pull-based gauge suffer from delays getting
// processed on the event queue of a `Process`, as well as incur
// computation cost on the `Process` each time the metrics are
// collected. Push-based gauges, on the other hand, incur no cost
// to the owning `Process` when metrics are collected, and instead
// incur a trivial cost when the `Process` pushes new values in.
class PushGauge : public Metric
{
public:
  // 'name' is the unique name for the instance of Gauge being constructed.
  // It will be the key exposed in the JSON endpoint.
  //
  // 'f' is the function that is called when the Metric value is requested.
  // The user of `Gauge` must ensure that `f` is safe to execute up until
  // the removal of the `Gauge` (via `process::metrics::remove(...)`) is
  // complete.
  explicit PushGauge(const std::string& name)
    : Metric(name, None()), data(new Data()) {}

  ~PushGauge() override {}

  Future<double> value() const override
  {
    return static_cast<double>(data->value.load());
  }

  PushGauge& operator=(double v)
  {
    data->value.store(v);
    push(v);
    return *this;
  }

  PushGauge& operator++() { return *this += 1; }

  PushGauge& operator+=(double v)
  {
    double prev;

    while (true) {
      prev = data->value.load();

      if (data->value.compare_exchange_weak(prev, prev + v)) {
        break;
      }
    }

    push(static_cast<double>(prev + v));
    return *this;
  }

  PushGauge& operator--() { return *this -= 1; }

  PushGauge& operator-=(double v)
  {
    double prev;

    while (true) {
      prev = data->value.load();

      if (data->value.compare_exchange_weak(prev, prev - v)) {
        break;
      }
    }

    push(static_cast<double>(prev - v));
    return *this;
  }

private:
  struct Data
  {
    explicit Data() : value(0) {}

    std::atomic<double> value;
  };

  std::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_PUSH_GAUGE_HPP__
