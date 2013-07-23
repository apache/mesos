#include <glog/logging.h>

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <process/clock.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>
#include <process/time.hpp>

#include <stout/error.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using namespace process;
using namespace process::http;

using std::list;
using std::map;
using std::string;
using std::vector;

namespace process {

// This is initialized by process::initialize().
Statistics* statistics = NULL;

const Duration STATISTICS_TRUNCATION_INTERVAL = Minutes(5);

// TODO(bmahler): Move these into timeseries.hpp header once we
// can require gcc >= 4.2.1.
const Duration TIME_SERIES_WINDOW = Weeks(2);
const size_t TIME_SERIES_CAPACITY = 1000;

class StatisticsProcess : public Process<StatisticsProcess>
{
public:
  StatisticsProcess(const Duration& _window, size_t _capacity)
    : ProcessBase("statistics"),
      window(_window),
      capacity(_capacity) {}

  virtual ~StatisticsProcess() {}

  // Statistics implementation.
  TimeSeries<double> timeseries(
      const string& context,
      const string& name);

  void set(
      const string& context,
      const string& name,
      double value,
      const Time& time);

  void increment(const string& context, const string& name);

  void decrement(const string& context, const string& name);

protected:
  virtual void initialize()
  {
    route("/snapshot.json", SNAPSHOT_HELP, &StatisticsProcess::snapshot);
    route("/series.json", SERIES_HELP, &StatisticsProcess::series);

    // Schedule the first truncation.
    delay(STATISTICS_TRUNCATION_INTERVAL, self(), &StatisticsProcess::truncate);
  }

private:
  static const string SNAPSHOT_HELP;
  static const string SERIES_HELP;

  // Removes values for all statistics that occurred outside the time
  // series window. We always ensure 1 value remains.
  // NOTE: Runs periodically every STATISTICS_TRUNCATION_INTERVAL.
  void truncate();

  // Returns the a snapshot of all statistics in JSON.
  Future<Response> snapshot(const Request& request);

  // Returns the time series of a statistic in JSON.
  Future<Response> series(const Request& request);

  const Duration window;
  const size_t capacity;

  // This maps from {context: {name: TimeSeries } }.
  hashmap<string, hashmap<string, TimeSeries<double> > > statistics;
};


const string StatisticsProcess::SERIES_HELP = HELP(
    TLDR(
        "Provides the time series for ..."),
    USAGE(
        "/statistics/series.json..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));


const string StatisticsProcess::SNAPSHOT_HELP = HELP(
    TLDR(
        "Provides a snapshot of the current statistics  ..."),
    USAGE(
        "/statistics/snapshot.json..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));


TimeSeries<double> StatisticsProcess::timeseries(
    const string& context,
    const string& name)
{
  if (!statistics.contains(context) || !statistics[context].contains(name)) {
    return TimeSeries<double>();
  }

  return statistics[context][name];
}


void StatisticsProcess::set(
    const string& context,
    const string& name,
    double value,
    const Time& time)
{
  if (!statistics[context].contains(name)) {
    statistics[context][name] = TimeSeries<double>(window, capacity);
  }
  statistics[context][name].set(value, time);
}


void StatisticsProcess::increment(const string& context, const string& name)
{
  double value = 0.0;
  if (statistics[context].contains(name) &&
      !statistics[context][name].empty()) {
    value = statistics[context][name].latest().get().data;
  }
  set(context, name, value + 1.0, Clock::now());
}


void StatisticsProcess::decrement(const string& context, const string& name)
{
  double value = 0.0;
  if (statistics[context].contains(name) &&
      !statistics[context][name].empty()) {
    value = statistics[context][name].latest().get().data;
  }
  set(context, name, value - 1.0, Clock::now());
}


void StatisticsProcess::truncate()
{
  foreachkey (const string& context, statistics) {
    foreachkey (const string& name, statistics[context]) {
      statistics[context][name].truncate();
    }
  }

  delay(STATISTICS_TRUNCATION_INTERVAL, self(), &StatisticsProcess::truncate);
}


Future<Response> StatisticsProcess::snapshot(const Request& request)
{
  JSON::Array array;

  Option<string> queryContext = request.query.get("context");
  Option<string> queryName = request.query.get("name");

  foreachkey (const string& context, statistics) {
    foreachkey (const string& name, statistics[context]) {
      // Skip statistics that don't match the query, if present.
      if (queryContext.isSome() && queryContext.get() != context) {
        continue;
      } else if (queryName.isSome() && queryName.get() != name) {
        continue;
      }

      const Option<TimeSeries<double>::Value>& value =
        statistics[context][name].latest();

      if (value.isSome()) {
        JSON::Object object;
        object.values["context"] = context;
        object.values["name"] = name;
        object.values["time"] = value.get().time.secs();
        object.values["value"] = value.get().data;
        array.values.push_back(object);
      }
    }
  }

  return OK(array, request.query.get("jsonp"));
}


Future<Response> StatisticsProcess::series(const Request& request)
{
  Option<string> context = request.query.get("context");
  Option<string> name = request.query.get("name");

  if (!context.isSome()) {
    return BadRequest("Expected 'context=val' in query.\n");
  } else if (!name.isSome()) {
    return BadRequest("Expected 'name=val' in query.\n");
  }

  Option<Time> start = None();
  Option<Time> stop = None();

  if (request.query.get("start").isSome()) {
    Try<double> result = numify<double>(request.query.get("start").get());
    if (result.isError()) {
      return BadRequest("Failed to parse 'start': " + result.error() + "\n.");
    }

    Try<Time> start_ = Time::create(result.get());
    if (start_.isError()) {
      return BadRequest("Failed to parse 'start': " + start_.error() + "\n.");
    }
    start = start_.get();
  }

  if (request.query.get("stop").isSome()) {
    Try<double> result = numify<double>(request.query.get("stop").get());
    if (result.isError()) {
      return BadRequest("Failed to parse 'stop': " + result.error() + "\n.");
    }

    Try<Time> stop_ = Time::create(result.get());
    if (stop_.isError()) {
      return BadRequest("Failed to parse 'stop': " + stop_.error() + "\n.");
    }
    stop = stop_.get();
  }

  if (start.isSome() && stop.isSome() && start.get() > stop.get()) {
    return BadRequest("Invalid query: 'start' must be less than 'stop'\n.");
  }

  JSON::Array array;

  const vector<TimeSeries<double>::Value>& values =
    timeseries(context.get(), name.get()).get(start, stop);

  foreach (const TimeSeries<double>::Value& value, values) {
    JSON::Object object;
    object.values["time"] = value.time.secs();
    object.values["value"] = value.data;
    array.values.push_back(object);
  }

  return OK(array, request.query.get("jsonp"));
}


Statistics::Statistics(const Duration& window, size_t capacity)
{
  process = new StatisticsProcess(window, capacity);
  spawn(process);
}


Statistics::~Statistics()
{
  terminate(process);
  wait(process);
}


Future<TimeSeries<double> > Statistics::timeseries(
    const string& context,
    const string& name)
{
  return dispatch(process, &StatisticsProcess::timeseries, context, name);
}


void Statistics::set(
    const string& context,
    const string& name,
    double value,
    const Time& time)
{
  dispatch(process, &StatisticsProcess::set, context, name, value, time);
}


void Statistics::increment(const string& context, const string& name)
{
  dispatch(process, &StatisticsProcess::increment, context, name);
}


void Statistics::decrement(const string& context, const string& name)
{
  dispatch(process, &StatisticsProcess::decrement, context, name);
}

} // namespace process {
