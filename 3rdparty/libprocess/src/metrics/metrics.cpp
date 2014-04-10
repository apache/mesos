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


// TODO(dhamon): Allow querying by context and context/name.
Future<http::Response> MetricsProcess::snapshot(const http::Request& request)
{
  hashmap<string, Future<double> > futures;

  foreachkey (const string& metric, metrics) {
    CHECK_NOTNULL(metrics[metric].get());
    futures[metric] = metrics[metric]->value();
  }

  return await(futures.values())
    .then(lambda::bind(_snapshot, request, futures));
}


Future<http::Response> MetricsProcess::_snapshot(
    const http::Request& request,
    const hashmap<string, Future<double> >& metrics)
{
  JSON::Object object;

  foreachpair (const string& key, const Future<double>& value, metrics) {
    if (value.isReady()) {
      object.values[key] = value.get();
    }
  }

  return http::OK(object, request.query.get("jsonp"));
}

}  // namespace internal {
}  // namespace metrics {
}  // namespace process {
