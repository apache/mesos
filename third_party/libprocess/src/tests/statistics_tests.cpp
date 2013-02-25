#include <gmock/gmock.h>

#include <map>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>

using namespace process;

using std::map;


TEST(Statistics, set)
{
  Statistics statistics(Days(1));

  // Set one using Clock::now() implicitly.
  statistics.set("test", "statistic", 3.0);

  // Set one using Clock::now() explicitly.
  Seconds now(Clock::now());
  statistics.set("test", "statistic", 4.0, now);

  Future<map<Seconds, double> > values = statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(2, values.get().size());

  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(3.0, values.get().begin()->second);

  EXPECT_EQ(1, values.get().count(now));
  EXPECT_DOUBLE_EQ(4.0, values.get()[now]);
}


TEST(Statistics, truncate)
{
  Clock::pause();

  Statistics statistics(Days(1));

  statistics.set("test", "statistic", 3.0);

  Future<map<Seconds, double> > values = statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(1, values.get().size());
  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(3.0, values.get().begin()->second);

  Clock::advance((60*60*24) + 1);

  statistics.increment("test", "statistic");

  values = statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(1, values.get().size());
  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(4.0, values.get().begin()->second);

  Clock::resume();
}


TEST(Statistics, meter) {
  Statistics statistics(Days(1));

  // Set up a meter, and ensure it captures the expected time rate.
  Future<Try<Nothing> > meter =
    statistics.meter("test", "statistic", new meters::TimeRate("metered"));

  meter.await();

  ASSERT_TRUE(meter.isReady());
  ASSERT_TRUE(meter.get().isSome());

  Seconds now(Clock::now());
  statistics.set("test", "statistic", 1.0, now);
  statistics.set("test", "statistic", 2.0, Seconds(now.secs() + 1.0));
  statistics.set("test", "statistic", 4.0, Seconds(now.secs() + 2.0));

  // Check the raw statistic values.
  Future<map<Seconds, double> > values = statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(3, values.get().size());
  EXPECT_EQ(1, values.get().count(now));
  EXPECT_EQ(1, values.get().count(Seconds(now.secs() + 1.0)));
  EXPECT_EQ(1, values.get().count(Seconds(now.secs() + 2.0)));

  EXPECT_EQ(1.0, values.get()[now]);
  EXPECT_EQ(2.0, values.get()[Seconds(now.secs() + 1.0)]);
  EXPECT_EQ(4.0, values.get()[Seconds(now.secs() + 2.0)]);

  // Now check the metered values.
  values = statistics.get("test", "metered");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(2, values.get().size());
  EXPECT_EQ(1, values.get().count(Seconds(now.secs() + 1.0)));
  EXPECT_EQ(1, values.get().count(Seconds(now.secs() + 2.0)));

  EXPECT_EQ(0., values.get()[now]);
  EXPECT_EQ(1.0, values.get()[Seconds(now.secs() + 1.0)]); // 100%.
  EXPECT_EQ(2.0, values.get()[Seconds(now.secs() + 2.0)]); // 200%.
}
