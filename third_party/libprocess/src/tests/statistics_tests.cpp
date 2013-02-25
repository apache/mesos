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
  Statistics statistics(Seconds(60*60*24));

  statistics.set("test", "statistic", 3.14);

  Future<map<Seconds, double> > values =
    statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(1, values.get().size());
  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(3.14, values.get().begin()->second);
}


TEST(Statistics, truncate)
{
  Clock::pause();

  Statistics statistics(Seconds(60*60*24));

  statistics.set("test", "statistic", 3.14);

  Future<map<Seconds, double> > values =
    statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(1, values.get().size());
  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(3.14, values.get().begin()->second);

  Clock::advance((60*60*24) + 1);

  statistics.increment("test", "statistic");

  values = statistics.get("test", "statistic");

  values.await();

  ASSERT_TRUE(values.isReady());
  EXPECT_EQ(1, values.get().size());
  EXPECT_GE(Clock::now(), values.get().begin()->first.secs());
  EXPECT_DOUBLE_EQ(4.14, values.get().begin()->second);

  Clock::resume();
}
