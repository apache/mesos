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

// Initializes the metrics library.
void initialize(const Option<std::string>& authenticationRealm = None());

namespace internal {

class MetricsProcess : public Process<MetricsProcess>
{
public:
  static MetricsProcess* instance();

  Future<Nothing> add(Owned<Metric> metric);

  Future<Nothing> remove(const std::string& name);

  Future<hashmap<std::string, double>> snapshot(
      const Option<Duration>& timeout);

protected:
  virtual void initialize();

private:
  static std::string help();

  MetricsProcess(
      const Option<Owned<RateLimiter>>& _limiter,
      const Option<std::string>& _authenticationRealm)
    : ProcessBase("metrics"),
      limiter(_limiter),
      authenticationRealm(_authenticationRealm)
  {}

  // Non-copyable, non-assignable.
  MetricsProcess(const MetricsProcess&);
  MetricsProcess& operator=(const MetricsProcess&);

  Future<http::Response> _snapshot(
      const http::Request& request,
      const Option<std::string>& /* principal */);

  static std::list<Future<double>> _snapshotTimeout(
      const std::list<Future<double>>& futures);

  static Future<hashmap<std::string, double>> __snapshot(
      const Option<Duration>& timeout,
      const hashmap<std::string, Future<double>>& metrics,
      const hashmap<std::string, Option<Statistics<double>>>& statistics);

  // The Owned<Metric> is an explicit copy of the Metric passed to 'add'.
  hashmap<std::string, Owned<Metric>> metrics;

  // Used to rate limit the snapshot endpoint.
  Option<Owned<RateLimiter>> limiter;

  // Needed for access to the private constructor.
  friend void process::metrics::initialize(
      const Option<std::string>& authenticationRealm);

  // The authentication realm that metrics HTTP endpoints are installed into.
  const Option<std::string> authenticationRealm;
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


inline Future<hashmap<std::string, double>> snapshot(
    const Option<Duration>& timeout)
{
  return dispatch(
      internal::MetricsProcess::instance(),
      &internal::MetricsProcess::snapshot,
      timeout);
}

}  // namespace metrics {
}  // namespace process {

#endif // __PROCESS_METRICS_METRICS_HPP__
