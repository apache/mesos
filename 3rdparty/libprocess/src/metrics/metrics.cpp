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

#include <glog/logging.h>

#include <list>
#include <string>
#include <vector>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/help.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

using std::list;
using std::string;
using std::vector;

namespace process {
namespace metrics {
namespace internal {

MetricsProcess* MetricsProcess::create(
    const Option<string>& authenticationRealm)
{
  Option<string> limit =
    os::getenv("LIBPROCESS_METRICS_SNAPSHOT_ENDPOINT_RATE_LIMIT");

  Option<Owned<RateLimiter>> limiter;

  // By default, we apply a rate limit of 2 requests
  // per second to the metrics snapshot endpoint in
  // order to maintain backwards compatibility (before
  // this was made configurable, we hard-coded a limit
  // of 2 requests per second).
  if (limit.isNone()) {
    limiter = Owned<RateLimiter>(new RateLimiter(2, Seconds(1)));
  } else if (limit->empty()) {
    limiter = None();
  } else {
    // TODO(vinod): Move this parsing logic to flags
    // once we have a 'Rate' abstraction in stout.
    Option<Error> reason;
    vector<string> tokens = strings::tokenize(limit.get(), "/");

    if (tokens.size() == 2) {
      Try<int> requests = numify<int>(tokens[0]);
      Try<Duration> interval = Duration::parse(tokens[1]);

      if (requests.isError()) {
        reason = Error(
            "Failed to parse the number of requests: " + requests.error());
      } else if (interval.isError()) {
        reason = Error(
            "Failed to parse the interval: " + interval.error());
      } else {
        limiter = Owned<RateLimiter>(
            new RateLimiter(requests.get(), interval.get()));
      }
    }

    if (limiter.isNone()) {
        EXIT(EXIT_FAILURE)
          << "Failed to parse LIBPROCESS_METRICS_SNAPSHOT_ENDPOINT_RATE_LIMIT "
          << "'" << limit.get() << "'"
          << " (format is <number of requests>/<interval duration>)"
          << (reason.isSome() ? ": " + reason.get().message : "");
    }
  }

  return new MetricsProcess(limiter, authenticationRealm);
}


void MetricsProcess::initialize()
{
  if (authenticationRealm.isSome()) {
    route("/snapshot",
          authenticationRealm.get(),
          help(),
          &MetricsProcess::_snapshot);
  } else {
    route("/snapshot",
          help(),
          [this](const http::Request& request) {
            return _snapshot(request, None());
          });
  }
}


string MetricsProcess::help()
{
  return HELP(
      TLDR("Provides a snapshot of the current metrics."),
      DESCRIPTION(
          "This endpoint provides information regarding the current metrics",
          "tracked by the system.",
          "",
          "The optional query parameter 'timeout' determines the maximum",
          "amount of time the endpoint will take to respond. If the timeout",
          "is exceeded, some metrics may not be included in the response.",
          "",
          "The key is the metric name, and the value is a double-type."),
      AUTHENTICATION(true));
}


Future<Nothing> MetricsProcess::add(Owned<Metric> metric)
{
  if (metrics.contains(metric->name())) {
    return Failure("Metric '" + metric->name() + "' was already added");
  }

  metrics[metric->name()] = metric;
  return Nothing();
}


Future<Nothing> MetricsProcess::remove(const string& name)
{
  if (!metrics.contains(name)) {
    return Failure("Metric '" + name + "' not found");
  }

  metrics.erase(name);

  return Nothing();
}


Future<hashmap<string, double>> MetricsProcess::snapshot(
    const Option<Duration>& timeout)
{
  hashmap<string, Future<double>> futures;
  hashmap<string, Option<Statistics<double>>> statistics;

  foreachkey (const string& metric, metrics) {
    CHECK_NOTNULL(metrics[metric].get());
    futures[metric] = metrics[metric]->value();
    // TODO(dhamon): It would be nice to compute these asynchronously.
    statistics[metric] = metrics[metric]->statistics();
  }

  if (timeout.isSome()) {
    return await(futures.values())
      .after(timeout.get(), lambda::bind(_snapshotTimeout, futures.values()))
      .then(lambda::bind(__snapshot, timeout, futures, statistics));
  } else {
    return await(futures.values())
      .then(lambda::bind(__snapshot, timeout, futures, statistics));
  }
}


Future<http::Response> MetricsProcess::_snapshot(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  // Parse the 'timeout' parameter.
  Option<Duration> timeout;

  if (request.url.query.contains("timeout")) {
    string parameter = request.url.query.get("timeout").get();

    Try<Duration> duration = Duration::parse(parameter);

    if (duration.isError()) {
      return http::BadRequest(
          "Invalid timeout '" + parameter + "': " + duration.error() + ".\n");
    }

    timeout = duration.get();
  }

  Future<Nothing> acquire = Nothing();

  if (limiter.isSome()) {
    acquire = limiter.get()->acquire();
  }

  return acquire.then(defer(self(), &Self::snapshot, timeout))
      .then([request](const hashmap<string, double>& metrics)
            -> http::Response {
        return http::OK(jsonify(metrics), request.url.query.get("jsonp"));
      });
}


list<Future<double>> MetricsProcess::_snapshotTimeout(
    const list<Future<double>>& futures)
{
  // Stop waiting for all futures to transition and return a 'ready'
  // list to proceed handling the request.
  return futures;
}


Future<hashmap<string, double>> MetricsProcess::__snapshot(
    const Option<Duration>& timeout,
    const hashmap<string, Future<double>>& metrics,
    const hashmap<string, Option<Statistics<double>>>& statistics)
{
  hashmap<string, double> snapshot;

  foreachpair (const string& key, const Future<double>& value, metrics) {
    // TODO(dhamon): Maybe add the failure message for this metric to the
    // response if value.isFailed().
    if (value.isPending()) {
      CHECK_SOME(timeout);
      VLOG(1) << "Exceeded timeout of " << timeout.get()
              << " when attempting to get metric '" << key << "'";
    } else if (value.isReady()) {
      snapshot[key] = value.get();
    }

    Option<Statistics<double>> statistics_ = statistics.get(key).get();

    if (statistics_.isSome()) {
      snapshot[key + "/count"] = static_cast<double>(statistics_.get().count);
      snapshot[key + "/min"] = statistics_.get().min;
      snapshot[key + "/max"] = statistics_.get().max;
      snapshot[key + "/p50"] = statistics_.get().p50;
      snapshot[key + "/p90"] = statistics_.get().p90;
      snapshot[key + "/p95"] = statistics_.get().p95;
      snapshot[key + "/p99"] = statistics_.get().p99;
      snapshot[key + "/p999"] = statistics_.get().p999;
      snapshot[key + "/p9999"] = statistics_.get().p9999;
    }
  }

  return snapshot;
}

}  // namespace internal {

}  // namespace metrics {
}  // namespace process {
