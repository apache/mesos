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

#include <sys/prctl.h>

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

namespace mesos {
namespace internal {
namespace tests {

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
  // Parse multiple cgroups with uint64 and floats.
  Try<hashmap<string, mesos::PerfStatistics>> parse =
    perf::parse("123,cycles,cgroup1\n"
                "456,cycles,cgroup2\n"
                "0.456,task-clock,cgroup2\n"
                "0.123,task-clock,cgroup1");

  ASSERT_SOME(parse);
  EXPECT_EQ(2u, parse->size());

  ASSERT_TRUE(parse->contains("cgroup1"));
  mesos::PerfStatistics statistics = parse->get("cgroup1").get();

  ASSERT_TRUE(statistics.has_cycles());
  EXPECT_EQ(123u, statistics.cycles());
  ASSERT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.123, statistics.task_clock());

  ASSERT_TRUE(parse->contains("cgroup2"));
  statistics = parse->get("cgroup2").get();

  ASSERT_TRUE(statistics.has_cycles());
  EXPECT_EQ(456u, statistics.cycles());
  EXPECT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.456, statistics.task_clock());

  // Statistics reporting <not supported> should not appear.
  parse = perf::parse("<not supported>,cycles,cgroup1");
  ASSERT_SOME(parse);

  ASSERT_TRUE(parse->contains("cgroup1"));
  statistics = parse->get("cgroup1").get();
  EXPECT_FALSE(statistics.has_cycles());

  // Statistics reporting <not counted> should be zero.
  parse = perf::parse("<not counted>,cycles,cgroup1\n"
                      "<not counted>,task-clock,cgroup1");
  ASSERT_SOME(parse);

  ASSERT_TRUE(parse->contains("cgroup1"));
  statistics = parse->get("cgroup1").get();

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
