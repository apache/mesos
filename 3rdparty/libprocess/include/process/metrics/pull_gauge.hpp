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

#ifndef __PROCESS_METRICS_PULL_GAUGE_HPP__
#define __PROCESS_METRICS_PULL_GAUGE_HPP__

#include <functional>
#include <memory>
#include <string>

#include <process/metrics/metric.hpp>

namespace process {
namespace metrics {

// A Metric that represents an instantaneous measurement of a value
// (e.g. number of items in a queue).
//
// A pull-based gauge differs from a push-based gauge in that the
// client does not need to worry about when to compute a new gauge
// value and instead provides the value function to invoke during
// metrics collection. This makes the client logic simpler but comes
// with significant performance disadvantages. If this function
// dispatches to a `Process`, metrics collection will be exposed to
// delays while the dispatches work their way through the event
// queues. Also, gauges can be expensive to compute (e.g. loop over
// all items while counting how many match a criteria) and therefore
// can lead to a performance degradation on critial `Process`es
// during metrics collection.
//
// NOTE: Due to the performance disadvantages of pull-based gauges,
// it is recommended to use push-based gauges where possible.
// Pull-based gauges should ideally (1) be used on `Process`es that
// experience very light load, and (2) use functions that do not
// perform heavyweight computation to compute the value (e.g. looping
// over a large collection).
class PullGauge : public Metric
{
public:
  // 'name' is the unique name for the instance of Gauge being constructed.
  // It will be the key exposed in the JSON endpoint.
  //
  // 'f' is the function that is called when the Metric value is requested.
  // The user of `Gauge` must ensure that `f` is safe to execute up until
  // the removal of the `Gauge` (via `process::metrics::remove(...)`) is
  // complete.
  PullGauge(const std::string& name, const std::function<Future<double>()>& f)
    : Metric(name, None()), data(new Data(f)) {}

  ~PullGauge() override {}

  Future<double> value() const override { return data->f(); }

private:
  struct Data
  {
    explicit Data(const std::function<Future<double>()>& _f) : f(_f) {}

    const std::function<Future<double>()> f;
  };

  std::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_PULL_GAUGE_HPP__
