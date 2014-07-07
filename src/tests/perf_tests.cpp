/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/stringify.hpp>

#include "linux/perf.hpp"

using std::set;
using std::string;

using namespace process;

class PerfTest : public ::testing::Test {};


TEST_F(PerfTest, ROOT_Events)
{
  set<string> events;
  // Valid events.
  events.insert("cycles");
  events.insert("task-clock");
  EXPECT_TRUE(perf::valid(events));

  // Add an invalid event.
  events.insert("this-is-an-invalid-event");
  EXPECT_FALSE(perf::valid(events));
}


TEST_F(PerfTest, Parse)
{
  // uint64 and floats should be parsed.
  Try<hashmap<string, mesos::PerfStatistics> > parse =
    perf::parse("123,cycles\n0.123,task-clock");
  CHECK_SOME(parse);

  ASSERT_TRUE(parse.get().contains(""));
  mesos::PerfStatistics statistics = parse.get().get("").get();

  ASSERT_TRUE(statistics.has_cycles());
  EXPECT_EQ(123u, statistics.cycles());
  ASSERT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.123, statistics.task_clock());

  // Parse multiple cgroups.
  parse = perf::parse("123,cycles,cgroup1\n"
                      "456,cycles,cgroup2\n"
                      "0.456,task-clock,cgroup2\n"
                      "0.123,task-clock,cgroup1");
  CHECK_SOME(parse);
  EXPECT_FALSE(parse.get().contains(""));

  ASSERT_TRUE(parse.get().contains("cgroup1"));
  statistics = parse.get().get("cgroup1").get();

  ASSERT_TRUE(statistics.has_cycles());
  EXPECT_EQ(123u, statistics.cycles());
  ASSERT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.123, statistics.task_clock());

  ASSERT_TRUE(parse.get().contains("cgroup2"));
  statistics = parse.get().get("cgroup2").get();

  ASSERT_TRUE(statistics.has_cycles());
  EXPECT_EQ(456u, statistics.cycles());
  EXPECT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.456, statistics.task_clock());

  // Statistics reporting <not supported> should not appear.
  parse = perf::parse("<not supported>,cycles");
  CHECK_SOME(parse);

  ASSERT_TRUE(parse.get().contains(""));
  statistics = parse.get().get("").get();
  EXPECT_FALSE(statistics.has_cycles());

  // Statistics reporting <not counted> should be zero.
  parse = perf::parse("<not counted>,cycles\n<not counted>,task-clock");
  CHECK_SOME(parse);

  ASSERT_TRUE(parse.get().contains(""));
  statistics = parse.get().get("").get();

  EXPECT_TRUE(statistics.has_cycles());
  EXPECT_EQ(0u, statistics.cycles());
  EXPECT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.0, statistics.task_clock());

  // Check parsing fails.
  parse = perf::parse("1,cycles\ngarbage");
  EXPECT_ERROR(parse);

  parse = perf::parse("1,unknown-field");
  EXPECT_ERROR(parse);
}


TEST_F(PerfTest, ROOT_SampleInit)
{
  set<string> events;
  // Hardware event.
  events.insert("cycles");
  // Software event.
  events.insert("task-clock");

  // Sample init/launchd/systemd (pid 1).
  Future<mesos::PerfStatistics> statistics =
    perf::sample(events, 1, Seconds(1));
  AWAIT_READY(statistics);

  // Check the sample timestamp is within the last 5 seconds. This is generous
  // because there's the process reap delay in addition to the sampling
  // duration.
  ASSERT_TRUE(statistics.get().has_timestamp());
  EXPECT_GT(
      Seconds(5).secs(), Clock::now().secs() - statistics.get().timestamp());
  EXPECT_EQ(Seconds(1).secs(), statistics.get().duration());

  ASSERT_TRUE(statistics.get().has_cycles());
  EXPECT_LT(0u, statistics.get().cycles());

  ASSERT_TRUE(statistics.get().has_task_clock());
  EXPECT_LT(0.0, statistics.get().task_clock());
}
