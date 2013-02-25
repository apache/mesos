#include <float.h> // For DBL_MAX.

#include <glog/logging.h>

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
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

  void set(
      const string& context,
      const string& name,
      double value,
      const Seconds& time);

  void increment(const string& context, const string& name);

  void decrement(const string& context, const string& name);

protected:
  virtual void initialize()
  {
    route("/snapshot.json", &StatisticsProcess::snapshot);
    route("/series.json", &StatisticsProcess::series);
  }

private:
  // Removes values for the specified statistic that occured outside
  // the time series window.
  void truncate(const string& context, const string& name);

  // Returns the a snapshot of all statistics in JSON.
  Future<Response> snapshot(const Request& request);

  // Returns the time series of a statistic in JSON.
  Future<Response> series(const Request& request);

  const Duration window;

  // We use a map instead of a hashmap to store the values because
  // that way we can retrieve a series in sorted order efficiently.
  hashmap<string, hashmap<string, map<Seconds, double> > > statistics;
};


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
    statistics[context].find(name)->second;

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
  // Update the raw value.
  statistics[context][name][time] = value;
  truncate(context, name);
}


void StatisticsProcess::increment(const string& context, const string& name)
{
  double value = 0.0;
  if (!statistics[context][name].empty()) {
    value = statistics[context][name].rbegin()->second;
  }
  set(context, name, value + 1.0, Seconds(Clock::now()));
}


void StatisticsProcess::decrement(const string& context, const string& name)
{
  double value = 0.0;
  if (!statistics[context][name].empty()) {
    value = statistics[context][name].rbegin()->second;
  }
  set(context, name, value - 1.0, Seconds(Clock::now()));
}


void StatisticsProcess::truncate(const string& context, const string& name)
{
  CHECK(statistics.contains(context));
  CHECK(statistics[context].contains(name));
  CHECK(statistics[context][name].size() > 0);

  // Always keep at least one value for a statistic.
  if (statistics[context][name].size() == 1) {
    return;
  }

  map<Seconds, double>::iterator start = statistics[context][name].begin();

  while ((Clock::now() - start->first.secs()) > window.secs()) {
    statistics[context][name].erase(start);
    if (statistics[context][name].size() == 1) {
      break;
    }
    start = statistics[context][name].begin();
  }
}


Future<Response> StatisticsProcess::snapshot(const Request& request)
{
  JSON::Array array;

  foreachkey (const string& context, statistics) {
    foreachkey (const string& name, statistics[context]) {
      CHECK(statistics[context][name].size() > 0);
      JSON::Object object;
      object.values["context"] = context;
      object.values["name"] = name;
      object.values["time"] = statistics[context][name].rbegin()->first.secs();
      object.values["value"] = statistics[context][name].rbegin()->second;
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
    start = Option<Seconds>::some(Seconds(result.get()));
  }

  if (request.query.get("stop").isSome()) {
    Try<double> result = numify<double>(request.query.get("stop").get());
    if (result.isError()) {
      return BadRequest("Failed to parse stop: " + result.error());
    }
    stop = Option<Seconds>::some(Seconds(result.get()));
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


void Statistics::set(
    const string& context,
    const string& name,
    double value,
    const Seconds& time)
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
