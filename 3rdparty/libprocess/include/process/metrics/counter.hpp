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

#ifndef __PROCESS_METRICS_COUNTER_HPP__
#define __PROCESS_METRICS_COUNTER_HPP__

#include <memory>
#include <string>

#include <process/metrics/metric.hpp>

namespace process {
namespace metrics {

// A Metric that represents an integer value that can be incremented and
// decremented.
class Counter : public Metric
{
public:
  // 'name' is the unique name for the instance of Counter being constructed.
  // This is what will be used as the key in the JSON endpoint.
  // 'window' is the amount of history to keep for this Metric.
  Counter(const std::string& name, const Option<Duration>& window = None())
    : Metric(name, window),
      data(new Data())
  {
    push(static_cast<double>(data->value.load()));
  }

  virtual ~Counter() {}

  virtual Future<double> value() const
  {
    return static_cast<double>(data->value.load());
  }

  void reset()
  {
    data->value.store(0);
    push(0);
  }

  Counter& operator++()
  {
    return *this += 1;
  }

  Counter operator++(int)
  {
    Counter c(*this);
    ++(*this);
    return c;
  }

  Counter& operator+=(int64_t v)
  {
    int64_t prev = data->value.fetch_add(v);
    push(static_cast<double>(prev + v));
    return *this;
  }

private:
  struct Data
  {
    explicit Data() : value(0) {}

    std::atomic<int64_t> value;
  };

  std::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_COUNTER_HPP__
