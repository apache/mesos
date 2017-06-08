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

#ifndef __PROCESS_METRICS_GAUGE_HPP__
#define __PROCESS_METRICS_GAUGE_HPP__

#include <functional>
#include <memory>
#include <string>

#include <process/metrics/metric.hpp>

namespace process {
namespace metrics {

// A Metric that represents an instantaneous value evaluated when
// 'value' is called.
class Gauge : public Metric
{
public:
  // 'name' is the unique name for the instance of Gauge being constructed.
  // It will be the key exposed in the JSON endpoint.
  //
  // 'f' is the function that is called when the Metric value is requested.
  // The user of `Gauge` must ensure that `f` is safe to execute up until
  // the removal of the `Gauge` (via `process::metrics::remove(...)`) is
  // complete.
  Gauge(const std::string& name, const std::function<Future<double>()>& f)
    : Metric(name, None()), data(new Data(f)) {}

  virtual ~Gauge() {}

  virtual Future<double> value() const { return data->f(); }

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

#endif // __PROCESS_METRICS_GAUGE_HPP__
