#include <float.h> // For DBL_MAX.

#include <glog/logging.h>

#include <map>
#include <string>
#include <vector>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/time.hpp>

using namespace process;
using namespace process::http;

using std::map;
using std::string;
using std::vector;

namespace process {

class StatisticsProcess : public Process<StatisticsProcess>
{
public:
  StatisticsProcess(const seconds& _window)
    : ProcessBase("statistics"),
      window(_window)
  {}

  virtual ~StatisticsProcess() {}

  // Statistics implementation.
  map<seconds, double> get(
      const string& name,
      const Option<seconds>& start,
      const Option<seconds>& stop);
  void set(const string& name, double value);
  void increment(const string& name);
  void decrement(const string& name);

protected:
  virtual void initialize()
  {
    route("snapshot.json", &StatisticsProcess::snapshot);
    route("series.json", &StatisticsProcess::series);
  }

private:
  // Removes values for the specified statistic that occured outside
  // the time series window.
  void truncate(const string& name);

  // Returns the a snapshot of all statistics in JSON.
  Future<Response> snapshot(const Request& request);

  // Returns the time series of a statistic in JSON.
  Future<Response> series(const Request& request);

  const seconds window;

  // We use a map instead of a hashmap to store the values because
  // that way we can retrieve a series in sorted order efficiently.
  hashmap<string, map<seconds, double> > statistics;
};


map<seconds, double> StatisticsProcess::get(
    const string& name,
    const Option<seconds>& start,
    const Option<seconds>& stop)
{
  if (!statistics.contains(name)) {
    return map<seconds, double>();
  }

  const std::map<seconds, double>& values = statistics.find(name)->second;

  map<seconds, double>::const_iterator lower =
    values.lower_bound(start.isSome() ? start.get() : seconds(0.0));

  map<seconds, double>::const_iterator upper =
    values.upper_bound(stop.isSome() ? stop.get() : seconds(DBL_MAX));

  return map<seconds, double>(lower, upper);
}


void StatisticsProcess::set(const string& name, double value)
{
  statistics[name][seconds(Clock::now())] = value;
  truncate(name);
}


void StatisticsProcess::increment(const string& name)
{
  if (statistics[name].size() > 0) {
    double d = statistics[name].rbegin()->second;
    statistics[name][seconds(Clock::now())] = d + 1.0;
  } else {
    statistics[name][seconds(Clock::now())] = 1.0;
  }

  truncate(name);
}


void StatisticsProcess::decrement(const string& name)
{
  if (statistics[name].size() > 0) {
    double d = statistics[name].rbegin()->second;
    statistics[name][seconds(Clock::now())] = d - 1.0;
  } else {
    statistics[name][seconds(Clock::now())] = -1.0;
  }

  truncate(name);
}


void StatisticsProcess::truncate(const string& name)
{
  CHECK(statistics.contains(name));
  CHECK(statistics[name].size() > 0);

  // Always keep at least value for a statistic.
  if (statistics[name].size() == 1) {
    return;
  }

  map<seconds, double>::iterator start = statistics[name].begin();

  while ((Clock::now() - start->first.value) > window.value) {
    statistics[name].erase(start);
    if (statistics[name].size() == 1) {
      break;
    }
    ++start;
  }
}


Future<Response> StatisticsProcess::snapshot(const Request& request)
{
  JSON::Array array;

  foreachkey (const string& name, statistics) {
    CHECK(statistics[name].size() > 0);
    JSON::Object object;
    object.values["name"] = name;
    object.values["time"] = statistics[name].rbegin()->first.value;
    object.values["value"] = statistics[name].rbegin()->second;
    array.values.push_back(object);
  }

  std::ostringstream out;

  JSON::render(out, array);

  OK response;
  response.headers["Content-Type"] = "application/json";
  response.headers["Content-Length"] = stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Future<Response> StatisticsProcess::series(const Request& request)
{
  // Get field=value pairs.
  map<string, vector<string> > pairs =
    strings::pairs(request.query, ";&", "=");

  Option<string> name = pairs.count("name") > 0 && pairs["name"].size() > 0
    ? Option<string>::some(pairs["name"].back())
    : Option<string>::none();

  Option<seconds> start = Option<seconds>::none();
  Option<seconds> stop = Option<seconds>::none();

  if (pairs.count("start") > 0 && pairs["start"].size() > 0) {
    Try<double> result = numify<double>(pairs["start"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'start' value (\""
                   << pairs["start"].back() << "\"): "
                   << result.error();
      return BadRequest();
    }
    start = Option<seconds>::some(seconds(result.get()));
  }

  if (pairs.count("stop") > 0 && pairs["stop"].size() > 0) {
    Try<double> result = numify<double>(pairs["stop"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'stop' value (\""
                   << pairs["stop"].back() << "\"): "
                   << result.error();
      return BadRequest();
    }
    stop = Option<seconds>::some(seconds(result.get()));
  }

  if (name.isSome()) {
    JSON::Array array;

    map<seconds, double> values = get(name.get(), start, stop);

    foreachpair (const seconds& s, double value, values) {
      JSON::Object object;
      object.values["time"] = s.value;
      object.values["value"] = value;
      array.values.push_back(object);
    }

    std::ostringstream out;

    JSON::render(out, array);

    OK response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Content-Length"] = stringify(out.str().size());
    response.body = out.str().data();
    return response;
  }

  return BadRequest();
}


Statistics::Statistics(const seconds& window)
{
  process = new StatisticsProcess(window);
  spawn(process);
}


Statistics::~Statistics()
{
  terminate(process);
  wait(process);
}


Future<map<seconds, double> > Statistics::get(
    const string& name,
    const Option<seconds>& start,
    const Option<seconds>& stop)
{
  return dispatch(process, &StatisticsProcess::get, name, start, stop);
}


void Statistics::set(const string& name, double value)
{
  dispatch(process, &StatisticsProcess::set, name, value);
}


void Statistics::increment(const string& name)
{
  dispatch(process, &StatisticsProcess::increment, name);
}


void Statistics::decrement(const string& name)
{
  dispatch(process, &StatisticsProcess::decrement, name);
}

} // namespace process {
