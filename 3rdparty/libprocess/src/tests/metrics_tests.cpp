#include <gtest/gtest.h>

#include <stout/gtest.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>


using process::Deferred;
using process::Failure;
using process::Future;
using process::PID;
using process::Process;

using process::metrics::add;
using process::metrics::remove;
using process::metrics::Counter;
using process::metrics::Gauge;


class GaugeProcess : public Process<GaugeProcess>
{
public:
  double get() { return 42.0; }
  Future<double> fail() { return Failure("failure"); }
};

// TODO(dhamon): Add test for JSON equality.
// TODO(dhamon): Add tests for JSON access with and without removal.

TEST(MetricsTest, Counter)
{
  Counter c("test/counter");
  AWAIT_READY(add(c));

  EXPECT_FLOAT_EQ(0.0, c.value().get());
  ++c;
  EXPECT_FLOAT_EQ(1.0, c.value().get());
  c++;
  EXPECT_FLOAT_EQ(2.0, c.value().get());

  c.reset();
  EXPECT_FLOAT_EQ(0.0, c.value().get());

  c += 42;
  EXPECT_FLOAT_EQ(42.0, c.value().get());

  AWAIT_READY(remove(c));
}


TEST(MetricsTest, Gauge)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  GaugeProcess process;
  PID<GaugeProcess> pid = spawn(&process);
  ASSERT_TRUE(pid);

  Gauge g("test/gauge", defer(pid, &GaugeProcess::get));
  Gauge g2("test/failedgauge", defer(pid, &GaugeProcess::fail));

  AWAIT_READY(add(g));
  AWAIT_READY(add(g2));

  AWAIT_READY(g.value());
  EXPECT_EQ(42.0, g.value().get());

  AWAIT_FAILED(g2.value());

  AWAIT_READY(remove(g2));
  AWAIT_READY(remove(g));

  terminate(process);
  wait(process);
}
