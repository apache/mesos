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

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/timer.hpp>

using namespace process;

using process::http::BadRequest;
using process::http::OK;
using process::http::Response;

using process::metrics::Counter;
using process::metrics::Gauge;
using process::metrics::Timer;

using std::map;
using std::string;

class GaugeProcess : public Process<GaugeProcess>
{
public:
  double get()
  {
    return 42.0;
  }

  Future<double> fail()
  {
    return Failure("failure");
  }

  Future<double> pending()
  {
    return Future<double>();
  }
};


TEST(MetricsTest, Counter)
{
  Counter counter("test/counter");

  AWAIT_READY(metrics::add(counter));

  AWAIT_EXPECT_EQ(0.0, counter.value());

  ++counter;
  AWAIT_EXPECT_EQ(1.0, counter.value());

  counter++;
  AWAIT_EXPECT_EQ(2.0, counter.value());

  counter.reset();
  AWAIT_EXPECT_EQ(0.0, counter.value());

  counter += 42;
  AWAIT_EXPECT_EQ(42.0, counter.value());

  EXPECT_NONE(counter.statistics());

  AWAIT_READY(metrics::remove(counter));
}


TEST(MetricsTest, Gauge)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  GaugeProcess process;
  PID<GaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  // Gauge with a value.
  Gauge gauge("test/gauge", defer(pid, &GaugeProcess::get));

  AWAIT_READY(metrics::add(gauge));

  AWAIT_EXPECT_EQ(42.0, gauge.value());

  AWAIT_READY(metrics::remove(gauge));

  // Failing gauge.
  gauge = Gauge("test/failedgauge", defer(pid, &GaugeProcess::fail));

  AWAIT_READY(metrics::add(gauge));

  AWAIT_EXPECT_FAILED(gauge.value());

  AWAIT_READY(metrics::remove(gauge));

  terminate(process);
  wait(process);
}


TEST(MetricsTest, Statistics)
{
  Counter counter("test/counter", process::TIME_SERIES_WINDOW);

  // We have to pause the clock to ensure the time series
  // entries are unique.
  Clock::pause();

  AWAIT_READY(metrics::add(counter));

  for (size_t i = 0; i < 10; ++i) {
    Clock::advance(Seconds(1));
    ++counter;
  }

  Option<Statistics<double> > statistics = counter.statistics();
  EXPECT_SOME(statistics);

  EXPECT_EQ(11u, statistics.get().count);

  EXPECT_FLOAT_EQ(0.0, statistics.get().min);
  EXPECT_FLOAT_EQ(10.0, statistics.get().max);

  EXPECT_FLOAT_EQ(5.0, statistics.get().p50);
  EXPECT_FLOAT_EQ(9.0, statistics.get().p90);
  EXPECT_FLOAT_EQ(9.5, statistics.get().p95);
  EXPECT_FLOAT_EQ(9.9, statistics.get().p99);
  EXPECT_FLOAT_EQ(9.99, statistics.get().p999);
  EXPECT_FLOAT_EQ(9.999, statistics.get().p9999);

  AWAIT_READY(metrics::remove(counter));
}


TEST(MetricsTest, Snapshot)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID upid("metrics", process::address());

  Clock::pause();

  // Add a gauge and a counter.
  GaugeProcess process;
  PID<GaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  Gauge gauge("test/gauge", defer(pid, &GaugeProcess::get));
  Gauge gaugeFail("test/gauge_fail", defer(pid, &GaugeProcess::fail));
  Counter counter("test/counter");

  AWAIT_READY(metrics::add(gauge));
  AWAIT_READY(metrics::add(gaugeFail));
  AWAIT_READY(metrics::add(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Get the snapshot.
  Future<Response> response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  Try<JSON::Object> responseJSON =
      JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(responseJSON);

  map<string, JSON::Value> values = responseJSON.get().values;

  EXPECT_EQ(1u, values.count("test/counter"));
  EXPECT_FLOAT_EQ(0.0, values["test/counter"].as<JSON::Number>().as<double>());

  EXPECT_EQ(1u, values.count("test/gauge"));
  EXPECT_FLOAT_EQ(42.0, values["test/gauge"].as<JSON::Number>().as<double>());

  EXPECT_EQ(0u, values.count("test/gauge_fail"));

  // Remove the metrics and ensure they are no longer in the snapshot.
  AWAIT_READY(metrics::remove(gauge));
  AWAIT_READY(metrics::remove(gaugeFail));
  AWAIT_READY(metrics::remove(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Ensure MetricsProcess has removed the metrics.
  Clock::settle();

  response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  responseJSON = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(responseJSON);

  values = responseJSON.get().values;
  EXPECT_EQ(0u, values.count("test/counter"));
  EXPECT_EQ(0u, values.count("test/gauge"));
  EXPECT_EQ(0u, values.count("test/gauge_fail"));

  terminate(process);
  wait(process);
}


TEST(MetricsTest, SnapshotTimeout)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID upid("metrics", process::address());

  Clock::pause();

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Ensure the timeout parameter is validated.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      http::get(upid, "snapshot", "timeout=foobar"));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Add gauges and a counter.
  GaugeProcess process;
  PID<GaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  Gauge gauge("test/gauge", defer(pid, &GaugeProcess::get));
  Gauge gaugeFail("test/gauge_fail", defer(pid, &GaugeProcess::fail));
  Gauge gaugeTimeout("test/gauge_timeout", defer(pid, &GaugeProcess::pending));
  Counter counter("test/counter");

  AWAIT_READY(metrics::add(gauge));
  AWAIT_READY(metrics::add(gaugeFail));
  AWAIT_READY(metrics::add(gaugeTimeout));
  AWAIT_READY(metrics::add(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Get the snapshot.
  Future<Response> response = http::get(upid, "snapshot", "timeout=2secs");

  // Make sure the request is pending before the timeout is exceeded.
  Clock::settle();

  ASSERT_TRUE(response.isPending());

  // Advance the clock to trigger the timeout.
  Clock::advance(Seconds(2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  Try<JSON::Object> responseJSON =
      JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(responseJSON);

  // We can't use simple JSON equality testing here as initializing
  // libprocess adds metrics to the system. We want to only check if
  // the metrics from this test are correctly handled.
  map<string, JSON::Value> values = responseJSON.get().values;

  EXPECT_EQ(1u, values.count("test/counter"));
  EXPECT_FLOAT_EQ(0.0, values["test/counter"].as<JSON::Number>().as<double>());

  EXPECT_EQ(1u, values.count("test/gauge"));
  EXPECT_FLOAT_EQ(42.0, values["test/gauge"].as<JSON::Number>().as<double>());

  EXPECT_EQ(0u, values.count("test/gauge_fail"));
  EXPECT_EQ(0u, values.count("test/gauge_timeout"));

  // Remove the metrics and ensure they are no longer in the snapshot.
  AWAIT_READY(metrics::remove(gauge));
  AWAIT_READY(metrics::remove(gaugeFail));
  AWAIT_READY(metrics::remove(gaugeTimeout));
  AWAIT_READY(metrics::remove(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Ensure MetricsProcess has removed the metrics.
  Clock::settle();

  response = http::get(upid, "snapshot", "timeout=2secs");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  responseJSON = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(responseJSON);

  values = responseJSON.get().values;

  ASSERT_SOME(responseJSON);
  EXPECT_EQ(0u, values.count("test/counter"));
  EXPECT_EQ(0u, values.count("test/gauge"));
  EXPECT_EQ(0u, values.count("test/gauge_fail"));
  EXPECT_EQ(0u, values.count("test/gauge_timeout"));

  terminate(process);
  wait(process);
}


// Ensures that the aggregate statistics are correct in the snapshot.
TEST(MetricsTest, SnapshotStatistics)
{
  UPID upid("metrics", process::address());

  Clock::pause();

  Counter counter("test/counter", process::TIME_SERIES_WINDOW);

  AWAIT_READY(metrics::add(counter));

  for (size_t i = 0; i < 10; ++i) {
    Clock::advance(Seconds(1));
    ++counter;
  }

  // We can't use simple JSON equality testing here as initializing
  // libprocess adds metrics to the system. We want to only check if
  // the metrics from this test are correctly handled.
  hashmap<string, double> expected;

  expected["test/counter"] = 10.0;

  expected["test/counter/count"] = 11;

  expected["test/counter/min"] = 0.0;
  expected["test/counter/max"] = 10.0;

  expected["test/counter/p50"] = 5.0;
  expected["test/counter/p90"] = 9.0;
  expected["test/counter/p95"] = 9.5;
  expected["test/counter/p99"] = 9.9;
  expected["test/counter/p999"] = 9.99;
  expected["test/counter/p9999"] = 9.999;

  Future<Response> response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> responseJSON =
      JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(responseJSON);

  // Pull the response values into a map.
  hashmap<string, double> responseValues;
  foreachpair (const string& key,
               const JSON::Value& value,
               responseJSON.get().values) {
    if (value.is<JSON::Number>()) {
      // "test/counter/count" is an integer, everything else is a double.
      JSON::Number number = value.as<JSON::Number>();
      responseValues[key] = number.as<double>();
    }
  }

  // Ensure the expected keys are in the response and that the values match
  // expectations.
  foreachkey (const string& key, expected) {
    EXPECT_FLOAT_EQ(expected[key], responseValues[key]);
  }

  AWAIT_READY(metrics::remove(counter));
}


TEST(MetricsTest, Timer)
{
  metrics::Timer<Nanoseconds> timer("test/timer");
  EXPECT_EQ("test/timer_ns", timer.name());

  AWAIT_READY(metrics::add(timer));

  // It is not an error to stop a timer that hasn't been started.
  timer.stop();

  // Time a no-op.
  Clock::pause();
  timer.start();
  Clock::advance(Microseconds(1));
  timer.stop();

  Future<double> value = timer.value();
  AWAIT_READY(value);
  EXPECT_FLOAT_EQ(value.get(), Microseconds(1).ns());

  // It is not an error to stop a timer that has already been stopped.
  timer.stop();

  AWAIT_READY(metrics::remove(timer));
}


static Future<int> advanceAndReturn()
{
  Clock::advance(Seconds(1));
  return 42;
}


TEST(MetricsTest, AsyncTimer)
{
  metrics::Timer<Microseconds> t("test/timer");
  EXPECT_EQ("test/timer_us", t.name());

  AWAIT_READY(metrics::add(t));

  // Time a Future that returns immediately. Even though the method
  // advances the clock and we advance the clock here, the Future
  // should be timed as if it takes 0 time. Ie, we're not timing the
  // method call but the Future.
  Clock::pause();
  Future<int> result = t.time(advanceAndReturn());
  Clock::advance(Seconds(1));
  AWAIT_READY(result);

  EXPECT_EQ(42, result.get());

  // The future should have taken zero time.
  AWAIT_READY(t.value());
  EXPECT_FLOAT_EQ(t.value().get(), 0.0);

  AWAIT_READY(metrics::remove(t));
}
