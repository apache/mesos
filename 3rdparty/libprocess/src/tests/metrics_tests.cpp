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

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <stout/base64.hpp>
#include <stout/duration.hpp>
#include <stout/gtest.hpp>

#include <process/authenticator.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/timer.hpp>

namespace authentication = process::http::authentication;
namespace http = process::http;
namespace metrics = process::metrics;

using authentication::Authenticator;
using authentication::BasicAuthenticator;

using http::BadRequest;
using http::OK;
using http::Response;
using http::Unauthorized;

using metrics::Counter;
using metrics::PullGauge;
using metrics::PushGauge;
using metrics::Timer;

using process::Clock;
using process::Failure;
using process::Future;
using process::PID;
using process::Process;
using process::Promise;
using process::READONLY_HTTP_AUTHENTICATION_REALM;
using process::Statistics;
using process::UPID;

using std::map;
using std::string;
using std::vector;

class PullGaugeProcess : public Process<PullGaugeProcess>
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
    return promise.future();
  }

  // Need to use a promise for the call to pending instead of just a
  // `Future<double>()` so we don't return an abandoned future.
  Promise<double> promise;
};


// TODO(greggomann): Move this into a base class in 'mesos.hpp'.
class MetricsTest : public ::testing::Test
{
protected:
  Future<Nothing> setAuthenticator(
      const string& realm,
      process::Owned<Authenticator> authenticator)
  {
    realms.insert(realm);

    return authentication::setAuthenticator(realm, authenticator);
  }

  void TearDown() override
  {
    foreach (const string& realm, realms) {
      // We need to wait in order to ensure that the operation
      // completes before we leave TearDown. Otherwise, we may
      // leak a mock object.
      AWAIT_READY(authentication::unsetAuthenticator(realm));
    }
    realms.clear();
  }

private:
  hashset<string> realms;
};


TEST_F(MetricsTest, Counter)
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


TEST_F(MetricsTest, PullGauge)
{
  PullGaugeProcess process;
  PID<PullGaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  // PullGauge with a value.
  PullGauge gauge("test/gauge", defer(pid, &PullGaugeProcess::get));

  AWAIT_READY(metrics::add(gauge));

  AWAIT_EXPECT_EQ(42.0, gauge.value());

  AWAIT_READY(metrics::remove(gauge));

  // Failing gauge.
  gauge = PullGauge("test/failedgauge", defer(pid, &PullGaugeProcess::fail));

  AWAIT_READY(metrics::add(gauge));

  AWAIT_EXPECT_FAILED(gauge.value());

  AWAIT_READY(metrics::remove(gauge));

  terminate(process);
  wait(process);
}


TEST_F(MetricsTest, PushGauge)
{
  // Gauge with a value.
  PushGauge gauge("test/gauge");

  AWAIT_READY(metrics::add(gauge));

  AWAIT_EXPECT_EQ(0.0, gauge.value());

  ++gauge;
  AWAIT_EXPECT_EQ(1.0, gauge.value());

  gauge += 42;
  AWAIT_EXPECT_EQ(43.0, gauge.value());

  --gauge;
  AWAIT_EXPECT_EQ(42.0, gauge.value());

  gauge -= 42;
  AWAIT_EXPECT_EQ(0.0, gauge.value());

  gauge = 42;
  AWAIT_EXPECT_EQ(42.0, gauge.value());

  EXPECT_NONE(gauge.statistics());

  AWAIT_READY(metrics::remove(gauge));
}


TEST_F(MetricsTest, Statistics)
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

  Option<Statistics<double>> statistics = counter.statistics();
  EXPECT_SOME(statistics);

  EXPECT_EQ(11u, statistics->count);

  EXPECT_DOUBLE_EQ(0.0, statistics->min);
  EXPECT_DOUBLE_EQ(10.0, statistics->max);

  EXPECT_DOUBLE_EQ(5.0, statistics->p50);
  EXPECT_DOUBLE_EQ(9.0, statistics->p90);
  EXPECT_DOUBLE_EQ(9.5, statistics->p95);
  EXPECT_DOUBLE_EQ(9.9, statistics->p99);
  EXPECT_DOUBLE_EQ(9.99, statistics->p999);
  EXPECT_DOUBLE_EQ(9.999, statistics->p9999);

  AWAIT_READY(metrics::remove(counter));
}


TEST_F(MetricsTest, THREADSAFE_Snapshot)
{
  UPID upid("metrics", process::address());

  Clock::pause();

  // Add a pull-gauge and a counter.
  PullGaugeProcess process;
  PID<PullGaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  PullGauge gauge("test/gauge", defer(pid, &PullGaugeProcess::get));
  PullGauge gaugeFail("test/gauge_fail", defer(pid, &PullGaugeProcess::fail));
  PullGauge gaugeConst("test/gauge_const", []() { return 99.0; });
  Counter counter("test/counter");

  AWAIT_READY(metrics::add(gauge));
  AWAIT_READY(metrics::add(gaugeFail));
  AWAIT_READY(metrics::add(gaugeConst));
  AWAIT_READY(metrics::add(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Get the snapshot.
  Future<Response> response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  Try<JSON::Object> responseJSON = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(responseJSON);

  map<string, JSON::Value> values = responseJSON->values;

  EXPECT_EQ(1u, values.count("test/counter"));
  EXPECT_DOUBLE_EQ(0.0, values["test/counter"].as<JSON::Number>().as<double>());

  EXPECT_EQ(1u, values.count("test/gauge"));
  EXPECT_DOUBLE_EQ(42.0, values["test/gauge"].as<JSON::Number>().as<double>());

  EXPECT_EQ(1u, values.count("test/gauge_const"));
  EXPECT_DOUBLE_EQ(
      99.0, values["test/gauge_const"].as<JSON::Number>().as<double>());

  EXPECT_EQ(0u, values.count("test/gauge_fail"));

  // Remove the metrics and ensure they are no longer in the snapshot.
  AWAIT_READY(metrics::remove(gauge));
  AWAIT_READY(metrics::remove(gaugeFail));
  AWAIT_READY(metrics::remove(gaugeConst));
  AWAIT_READY(metrics::remove(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Ensure MetricsProcess has removed the metrics.
  Clock::settle();

  response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  responseJSON = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(responseJSON);

  values = responseJSON->values;
  EXPECT_EQ(0u, values.count("test/counter"));
  EXPECT_EQ(0u, values.count("test/gauge"));
  EXPECT_EQ(0u, values.count("test/gauge_fail"));
  EXPECT_EQ(0u, values.count("test/gauge_const"));

  terminate(process);
  wait(process);
}


// Ensure the response string has the JSON keys sorted
// alphabetically, we do this for easier human consumption.
TEST_F(MetricsTest, SnapshotAlphabetical)
{
  UPID upid("metrics", process::address());

  Clock::pause();

  vector<Counter> counters = {
    Counter("test/f"),
    Counter("test/e"),
    Counter("test/d"),
    Counter("test/c"),
    Counter("test/b"),
    Counter("test/a"),
  };

  foreach (const Counter& counter, counters) {
    AWAIT_READY(metrics::add(counter));
  }

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Get the snapshot.
  Future<Response> response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Ensure the response is ordered alphabetically.
  EXPECT_LT(response->body.find("test/e"),
            response->body.find("test/f"));

  EXPECT_LT(response->body.find("test/d"),
            response->body.find("test/e"));

  EXPECT_LT(response->body.find("test/c"),
            response->body.find("test/d"));

  EXPECT_LT(response->body.find("test/b"),
            response->body.find("test/c"));

  EXPECT_LT(response->body.find("test/a"),
            response->body.find("test/b"));

  foreach (const Counter& counter, counters) {
    AWAIT_READY(metrics::remove(counter));
  }
}


TEST_F(MetricsTest, THREADSAFE_SnapshotTimeout)
{
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

  // Add pull-gauges and a counter.
  PullGaugeProcess process;
  PID<PullGaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  PullGauge gauge(
      "test/gauge",
      defer(pid, &PullGaugeProcess::get));
  PullGauge gaugeFail(
      "test/gauge_fail",
      defer(pid, &PullGaugeProcess::fail));
  PullGauge gaugeTimeout(
      "test/gauge_timeout",
      defer(pid, &PullGaugeProcess::pending));
  Counter counter(
      "test/counter");

  AWAIT_READY(metrics::add(gauge));
  AWAIT_READY(metrics::add(gaugeFail));
  AWAIT_READY(metrics::add(gaugeTimeout));
  AWAIT_READY(metrics::add(counter));

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // Get the snapshot.
  Future<Response> response = http::get(upid, "snapshot", "timeout=2secs");

  // Make sure the request is pending before the timeout is exceeded.
  //
  // TODO(neilc): Replace the `sleep` here with a less flaky
  // synchronization method.
  os::sleep(Milliseconds(10));
  Clock::settle();

  ASSERT_TRUE(response.isPending());

  // Advance the clock to trigger the timeout.
  Clock::advance(Seconds(2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Parse the response.
  Try<JSON::Object> responseJSON = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(responseJSON);

  // We can't use simple JSON equality testing here as initializing
  // libprocess adds metrics to the system. We want to only check if
  // the metrics from this test are correctly handled.
  map<string, JSON::Value> values = responseJSON->values;

  EXPECT_EQ(1u, values.count("test/counter"));
  EXPECT_DOUBLE_EQ(0.0, values["test/counter"].as<JSON::Number>().as<double>());

  EXPECT_EQ(1u, values.count("test/gauge"));
  EXPECT_DOUBLE_EQ(42.0, values["test/gauge"].as<JSON::Number>().as<double>());

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
  responseJSON = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(responseJSON);

  values = responseJSON->values;

  ASSERT_SOME(responseJSON);
  EXPECT_EQ(0u, values.count("test/counter"));
  EXPECT_EQ(0u, values.count("test/gauge"));
  EXPECT_EQ(0u, values.count("test/gauge_fail"));
  EXPECT_EQ(0u, values.count("test/gauge_timeout"));

  terminate(process);
  wait(process);
}


// Ensures that the aggregate statistics are correct in the snapshot.
TEST_F(MetricsTest, SnapshotStatistics)
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

  Try<JSON::Object> responseJSON = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(responseJSON);

  // Pull the response values into a map.
  hashmap<string, double> responseValues;
  foreachpair (const string& key,
               const JSON::Value& value,
               responseJSON->values) {
    if (value.is<JSON::Number>()) {
      // "test/counter/count" is an integer, everything else is a double.
      JSON::Number number = value.as<JSON::Number>();
      responseValues[key] = number.as<double>();
    }
  }

  // Ensure the expected keys are in the response and that the values match
  // expectations.
  foreachkey (const string& key, expected) {
    EXPECT_DOUBLE_EQ(expected[key], responseValues[key]);
  }

  AWAIT_READY(metrics::remove(counter));
}


TEST_F(MetricsTest, Timer)
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
  EXPECT_DOUBLE_EQ(value.get(), static_cast<double>(Microseconds(1).ns()));

  // It is not an error to stop a timer that has already been stopped.
  timer.stop();

  AWAIT_READY(metrics::remove(timer));
}


static Future<int> advanceAndReturn()
{
  Clock::advance(Seconds(1));
  return 42;
}


TEST_F(MetricsTest, AsyncTimer)
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
  EXPECT_DOUBLE_EQ(t.value().get(), 0.0);

  AWAIT_READY(metrics::remove(t));
}


// Tests that the `/metrics/snapshot` endpoint rejects unauthenticated requests
// when HTTP authentication is enabled.
TEST_F(MetricsTest, THREADSAFE_SnapshotAuthenticationEnabled)
{
  process::Owned<Authenticator> authenticator(
    new BasicAuthenticator(
        READONLY_HTTP_AUTHENTICATION_REALM, {{"foo", "bar"}}));

  AWAIT_READY(
      setAuthenticator(READONLY_HTTP_AUTHENTICATION_REALM, authenticator));

  UPID upid("metrics", process::address());

  Clock::pause();

  // Advance the clock to avoid rate limit.
  Clock::advance(Seconds(1));

  // A request with no authentication header.
  Future<Response> response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}
