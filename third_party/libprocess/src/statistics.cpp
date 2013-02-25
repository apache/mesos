#include <float.h> // For DBL_MAX.

#include <glog/logging.h>

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <process/clock.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>

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

// TODO(bmahler): Move time series related logic into this struct.
// TODO(bmahler): Investigate using google's btree implementation.
// This provides better insertion and lookup performance for large
// containers. This _should_ also provide significant memory
// savings, especially since:
//   1. Our insertion order will mostly be in sorted order.
//   2. Our keys (Seconds) have efficient comparison operators.
// See: http://code.google.com/p/cpp-btree/
//      http://code.google.com/p/cpp-btree/wiki/UsageInstructions
struct TimeSeries
{
  TimeSeries() : values(), archived(false) {}

  // We use a map instead of a hashmap to store the values because
  // that way we can retrieve a series in sorted order efficiently.
  map<Seconds, double> values;
  bool archived;
};


class StatisticsProcess : public Process<StatisticsProcess>
{
public:
  StatisticsProcess(const Duration& _window)
    : ProcessBase("statistics"),
      window(_window) {}

  virtual ~StatisticsProcess() {}

  // Statistics implementation.
  map<Seconds, double> get(
      const string& context,
      const string& name,
      const Option<Seconds>& start,
      const Option<Seconds>& stop);

  Try<Nothing> meter(
      const string& context,
      const string& name,
      const Owned<meters::Meter>& meter);

  void set(
      const string& context,
      const string& name,
      double value,
      const Seconds& time);

  void archive(const string& context, const string& name);

  void increment(const string& context, const string& name);

  void decrement(const string& context, const string& name);

protected:
  virtual void initialize()
  {
    route("/snapshot.json", &StatisticsProcess::snapshot);
    route("/series.json", &StatisticsProcess::series);

    // Schedule the first truncation.
    delay(STATISTICS_TRUNCATION_INTERVAL, self(), &StatisticsProcess::truncate);
  }

private:
  // Removes values for the specified statistic that occurred outside
  // the time series window.
  // NOTE: We always ensure there is at least 1 value left for a statistic,
  // unless it is archived!
  // Returns true iff the time series is empty.
  bool truncate(const string& context, const string& name);

  // Removes values for all statistics that occurred outside the time
  // series window.
  // NOTE: Runs perpetually based on the STATISTICS_TRUNCATION_INTERVAL.
  // NOTE: We always ensure there is at least 1 value left for a statistic,
  // unless it is archived.
  void truncate();

  // Returns the a snapshot of all statistics in JSON.
  Future<Response> snapshot(const Request& request);

  // Returns the time series of a statistic in JSON.
  Future<Response> series(const Request& request);

  const Duration window;

  // This maps from {context: {name: TimeSeries } }.
  hashmap<string, hashmap<string, TimeSeries> > statistics;

  // Each statistic can have many meters.
  // This maps from {context: {name: [meters] } }.
  hashmap<string, hashmap<string, list<Owned<meters::Meter> > > > meters;
};


Try<Nothing> StatisticsProcess::meter(
    const string& context,
    const string& name,
    const Owned<meters::Meter>& meter)
{
  if (meter->name == name) {
    return Error("Meter name must not match the statistic name");
  }

  // Check for a duplicate meter.
  foreachkey (const string& context, meters) {
    foreachkey (const string& name, meters[context]) {
      foreach (Owned<meters::Meter>& existing, meters[context][name]) {
        if (meter->name == existing->name) {
          return Error("Meter name matched existing meter name");
        }
      }
    }
  }

  // Add the meter.
  meters[context][name].push_back(meter);

  return Nothing();
}


map<Seconds, double> StatisticsProcess::get(
    const string& context,
    const string& name,
    const Option<Seconds>& start,
    const Option<Seconds>& stop)
{
  if (!statistics.contains(context) || !statistics[context].contains(name)) {
    return map<Seconds, double>();
  }

  const std::map<Seconds, double>& values =
    statistics[context].find(name)->second.values;

  map<Seconds, double>::const_iterator lower =
    values.lower_bound(start.isSome() ? start.get() : Seconds(0.0));

  map<Seconds, double>::const_iterator upper =
    values.upper_bound(stop.isSome() ? stop.get() : Seconds(DBL_MAX));

  return map<Seconds, double>(lower, upper);
}


void StatisticsProcess::set(
    const string& context,
    const string& name,
    double value,
    const Seconds& time)
{
  statistics[context][name].values[time] = value; // Update the raw value.
  statistics[context][name].archived = false;     // Unarchive.

  truncate(context, name);

  // Update the metered values, if necessary.
  if (meters.contains(context) && meters[context].contains(name)) {
    foreach (Owned<meters::Meter>& meter, meters[context][name]) {
      const Option<double>& update = meter->update(time, value);
      statistics[context][meter->name].archived = false; // Unarchive.

      if (update.isSome()) {
        statistics[context][meter->name].values[time] = update.get();
        truncate(context, meter->name);
      }
    }
  }
}


void StatisticsProcess::archive(const string& context, const string& name)
{
  // Exclude the statistic from the snapshot.
  statistics[context][name].archived = true;

  // Remove any meters as well.
  if (meters.contains(context) && meters[context].contains(name)) {
    foreach (const Owned<meters::Meter>& meter, meters[context][name]) {
      statistics[context][meter->name].archived = true;
    }
    meters[context].erase(name);
  }
}


void StatisticsProcess::increment(const string& context, const string& name)
{
  double value = 0.0;
  if (!statistics[context][name].values.empty()) {
    value = statistics[context][name].values.rbegin()->second;
  }
  set(context, name, value + 1.0, Seconds(Clock::now()));
}


void StatisticsProcess::decrement(const string& context, const string& name)
{
  double value = 0.0;
  if (!statistics[context][name].values.empty()) {
    value = statistics[context][name].values.rbegin()->second;
  }
  set(context, name, value - 1.0, Seconds(Clock::now()));
}


bool StatisticsProcess::truncate(const string& context, const string& name)
{
  CHECK(statistics.contains(context));
  CHECK(statistics[context].contains(name));

  if (statistics[context][name].values.empty()) {
    return true; // No truncation is needed, the time series is already empty.
  }

  map<Seconds, double>::iterator start =
    statistics[context][name].values.begin();

  while ((Clock::now() - start->first.secs()) > window.secs()) {
    // Always keep at least one value for a statistic, unless it's archived!
    if (statistics[context][name].values.size() == 1) {
      if (statistics[context][name].archived) {
        statistics[context][name].values.clear();
      }
      break;
    }

    statistics[context][name].values.erase(start);
    start = statistics[context][name].values.begin();
  }

  return statistics[context][name].values.empty();
}


void StatisticsProcess::truncate()
{
  hashmap<string, hashset<string> > empties;

  foreachkey (const string& context, statistics) {
    foreachkey (const string& name, statistics[context]) {
      // Keep track of the emptied timeseries.
      if (truncate(context, name)) {
        empties[context].insert(name);
      }
    }
  }

  // Remove the empty timeseries.
  foreachkey (const string& context, empties) {
    foreach (const string& name, empties[context]) {
      statistics[context].erase(name);
    }
  }

  delay(STATISTICS_TRUNCATION_INTERVAL, self(), &StatisticsProcess::truncate);
}


Future<Response> StatisticsProcess::snapshot(const Request& request)
{
  JSON::Array array;

  foreachkey (const string& context, statistics) {
    foreachkey (const string& name, statistics[context]) {
      // Exclude archived time series.
      if (statistics[context][name].archived) {
        continue;
      }

      CHECK(statistics[context][name].values.size() > 0);

      JSON::Object object;
      object.values["context"] = context;
      object.values["name"] = name;
      object.values["time"] =
        statistics[context][name].values.rbegin()->first.secs();
      object.values["value"] =
        statistics[context][name].values.rbegin()->second;
      array.values.push_back(object);
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

  Option<Seconds> start = None();
  Option<Seconds> stop = None();

  if (request.query.get("start").isSome()) {
    Try<double> result = numify<double>(request.query.get("start").get());
    if (result.isError()) {
      return BadRequest("Failed to parse start: " + result.error());
    }
    start = Seconds(result.get());
  }

  if (request.query.get("stop").isSome()) {
    Try<double> result = numify<double>(request.query.get("stop").get());
    if (result.isError()) {
      return BadRequest("Failed to parse stop: " + result.error());
    }
    stop = Seconds(result.get());
  }

  JSON::Array array;

  map<Seconds, double> values = get(context.get(), name.get(), start, stop);

  foreachpair (const Seconds& s, double value, values) {
    JSON::Object object;
    object.values["time"] = s.secs();
    object.values["value"] = value;
    array.values.push_back(object);
  }

  return OK(array, request.query.get("jsonp"));
}


Statistics::Statistics(const Duration& window)
{
  process = new StatisticsProcess(window);
  spawn(process);
}


Statistics::~Statistics()
{
  terminate(process);
  wait(process);
}


Future<map<Seconds, double> > Statistics::get(
    const string& context,
    const string& name,
    const Option<Seconds>& start,
    const Option<Seconds>& stop)
{
  return dispatch(process, &StatisticsProcess::get, context, name, start, stop);
}


Future<Try<Nothing> > Statistics::meter(
    const string& context,
    const string& name,
    Owned<meters::Meter> meter)
{

  return dispatch(process, &StatisticsProcess::meter, context, name, meter);
}


void Statistics::set(
    const string& context,
    const string& name,
    double value,
    const Seconds& time)
{
  dispatch(process, &StatisticsProcess::set, context, name, value, time);
}


void Statistics::archive(const string& context, const string& name)
{
  dispatch(process, &StatisticsProcess::archive, context, name);
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
