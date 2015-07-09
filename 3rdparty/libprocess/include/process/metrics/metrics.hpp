/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#ifndef __PROCESS_METRICS_METRICS_HPP__
#define __PROCESS_METRICS_METRICS_HPP__

#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/metrics/metric.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

namespace process {
namespace metrics {

namespace internal {

class MetricsProcess : public Process<MetricsProcess>
{
public:
  static MetricsProcess* instance();

  Future<Nothing> add(Owned<Metric> metric);

  Future<Nothing> remove(const std::string& name);

protected:
  virtual void initialize();

private:
  static std::string help();

  MetricsProcess()
    : ProcessBase("metrics"),
      limiter(2, Seconds(1))
  {}

  // Non-copyable, non-assignable.
  MetricsProcess(const MetricsProcess&);
  MetricsProcess& operator = (const MetricsProcess&);

  Future<http::Response> snapshot(const http::Request& request);
  Future<http::Response> _snapshot(const http::Request& request);
  static std::list<Future<double> > _snapshotTimeout(
      const std::list<Future<double> >& futures);
  static Future<http::Response> __snapshot(
      const http::Request& request,
      const Option<Duration>& timeout,
      const hashmap<std::string, Future<double> >& metrics,
      const hashmap<std::string, Option<Statistics<double> > >& statistics);

  // The Owned<Metric> is an explicit copy of the Metric passed to 'add'.
  hashmap<std::string, Owned<Metric> > metrics;

  // Used to rate limit the endpoint.
  RateLimiter limiter;
};

}  // namespace internal {


template <typename T>
Future<Nothing> add(const T& metric)
{
  // There is an explicit copy in this call to ensure we end up owning
  // the last copy of a Metric when we remove it.
  return dispatch(
      internal::MetricsProcess::instance(),
      &internal::MetricsProcess::add,
      Owned<Metric>(new T(metric)));
}


inline Future<Nothing> remove(const Metric& metric)
{
  return dispatch(
      internal::MetricsProcess::instance(),
      &internal::MetricsProcess::remove,
      metric.name());
}

}  // namespace metrics {
}  // namespace process {

#endif // __PROCESS_METRICS_METRICS_HPP__
