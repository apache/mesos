#include <glog/logging.h>

#include <list>
#include <string>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/help.hpp>
#include <process/once.hpp>
#include <process/process.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>

using std::list;
using std::string;

namespace process {
namespace metrics {
namespace internal {

MetricsProcess* MetricsProcess::instance()
{
  static MetricsProcess* singleton = NULL;
  static Once* initialized = new Once();

  if (!initialized->once()) {
    singleton = new MetricsProcess();
    spawn(singleton);
    initialized->done();
  }

  return singleton;
}


void MetricsProcess::initialize()
{
  route("/snapshot", help(), &MetricsProcess::snapshot);
}


string MetricsProcess::help()
{
  return HELP(
      TLDR("Provides a snapshot of the current metrics."),
      USAGE("/metrics/snapshot"),
      DESCRIPTION(
          "This endpoint provides information regarding the current metrics ",
          "tracked by the system.",
          "",
          "The key is the metric name, and the value is a double-type."));
}


Future<Nothing> MetricsProcess::add(Owned<Metric> metric)
{
  if (metrics.contains(metric->name())) {
    return Failure("Metric '" + metric->name() + "' was already added.");
  }

  metrics[metric->name()] = metric;
  return Nothing();
}


Future<Nothing> MetricsProcess::remove(const std::string& name)
{
  if (!metrics.contains(name)) {
    return Failure("Metric '" + name + "' not found.");
  }

  metrics.erase(name);

  return Nothing();
}


Future<http::Response> MetricsProcess::snapshot(const http::Request& request)
{
  return limiter.acquire()
    .then(defer(self(), &Self::_snapshot, request));
}


Future<http::Response> MetricsProcess::_snapshot(const http::Request& request)
{
  hashmap<string, Future<double> > futures;
  hashmap<string, Option<Statistics<double> > > statistics;

  foreachkey (const string& metric, metrics) {
    CHECK_NOTNULL(metrics[metric].get());
    futures[metric] = metrics[metric]->value();
    // TODO(dhamon): It would be nice to get these in parallel.
    statistics[metric] = metrics[metric]->statistics();
  }

  return await(futures.values())
    .then(lambda::bind(__snapshot, request, futures, statistics));
}


Future<http::Response> MetricsProcess::__snapshot(
    const http::Request& request,
    const hashmap<string, Future<double> >& metrics,
    const hashmap<string, Option<Statistics<double> > >& statistics)
{
  JSON::Object object;

  foreachpair (const string& key, const Future<double>& value, metrics) {
    // Value.
    if (value.isReady()) {
      object.values[key] = value.get();
    }

    // Statistics.
    Option<Statistics<double> > statistics_ = statistics.get(key).get();

    if (statistics_.isSome()) {
      object.values[key + "/min"] = statistics_.get().min;
      object.values[key + "/max"] = statistics_.get().max;
      object.values[key + "/p50"] = statistics_.get().p50;
      object.values[key + "/p90"] = statistics_.get().p90;
      object.values[key + "/p95"] = statistics_.get().p95;
      object.values[key + "/p99"] = statistics_.get().p99;
      object.values[key + "/p999"] = statistics_.get().p999;
      object.values[key + "/p9999"] = statistics_.get().p9999;
    }
  }

  return http::OK(object, request.query.get("jsonp"));
}

}  // namespace internal {
}  // namespace metrics {
}  // namespace process {
