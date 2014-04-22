#include <gtest/gtest.h>

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

using process::http::OK;
using process::http::Response;

using process::metrics::Counter;
using process::metrics::Gauge;
using process::metrics::Timer;


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
};


TEST(Metrics, Counter)
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


TEST(Metrics, Gauge)
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


TEST(Metrics, Statistics)
{
  Counter counter("test/counter", process::TIME_SERIES_WINDOW);

  AWAIT_READY(metrics::add(counter));

  for (size_t i = 0; i < 10; ++i) {
    ++counter;
  }

  Option<Statistics<double> > statistics = counter.statistics();
  EXPECT_SOME(statistics);

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


TEST(Metrics, Snapshot)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID upid("metrics", process::ip(), process::port());

  // Before adding any metrics, the response should be empty.
  Future<Response> response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(JSON::Object()), response);

  // Add a gauge and a counter.
  GaugeProcess process;
  PID<GaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  Gauge gauge("test/gauge", defer(pid, &GaugeProcess::get));
  Counter counter("test/counter");

  AWAIT_READY(metrics::add(gauge));
  AWAIT_READY(metrics::add(counter));

  // Get the snapshot.
  response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  JSON::Object expected;
  expected.values["test/counter"] = 0.0;
  expected.values["test/gauge"] = 42.0;
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Remove the metrics and ensure they are no longer in the snapshot.
  AWAIT_READY(metrics::remove(gauge));
  AWAIT_READY(metrics::remove(counter));

  response = http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(JSON::Object()), response);

  terminate(process);
  wait(process);
}


// Ensures that the aggregate statistics are correct in the snapshot.
TEST(Metrics, SnapshotStatistics)
{
  UPID upid("metrics", process::ip(), process::port());

  Future<Response> response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(JSON::Object()), response);

  Clock::pause();

  Counter counter("test/counter", process::TIME_SERIES_WINDOW);

  AWAIT_READY(metrics::add(counter));

  for (size_t i = 0; i < 10; ++i) {
    Clock::advance(Seconds(1));
    ++counter;
  }

  JSON::Object expected;

  expected.values["test/counter"] = 10.0;

  expected.values["test/counter/min"] = 0.0;
  expected.values["test/counter/max"] = 10.0;

  expected.values["test/counter/p50"] = 5.0;
  expected.values["test/counter/p90"] = 9.0;
  expected.values["test/counter/p95"] = 9.5;
  expected.values["test/counter/p99"] = 9.9;
  expected.values["test/counter/p999"] = 9.99;
  expected.values["test/counter/p9999"] = 9.999;

  response = http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  AWAIT_READY(metrics::remove(counter));
}


TEST(MetricsTest, Timer)
{
  metrics::Timer timer("test/timer");

  AWAIT_READY(metrics::add(timer));

  // It is not an error to stop a timer that hasn't been started.
  timer.stop();

  // Time a no-op.
  Time started = Clock::now();

  timer.start();
  timer.stop();

  Time stopped = Clock::now();

  Future<double> value = timer.value();
  AWAIT_READY(value);
  EXPECT_LE(value.get(), (stopped - started).ms());

  // Make sure that re-starting a timer records the correct value.
  timer.start();

  started = Clock::now();

  timer.start();
  timer.stop();

  stopped = Clock::now();

  value = timer.value();
  AWAIT_READY(value);
  EXPECT_LE(value.get(), (stopped - started).ms());

  // It is not an error to stop a timer that has already been stopped.
  timer.stop();

  AWAIT_READY(metrics::remove(timer));
}
