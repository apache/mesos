// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/prctl.h>

#include <set>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include <stout/os/shell.hpp>

#include "common/status_utils.hpp"

#include "linux/perf.hpp"

using std::set;
using std::string;

using namespace process;

namespace mesos {
namespace internal {
namespace tests {

class PerfTest : public ::testing::Test {};


TEST_F(PerfTest, ROOT_PERF_Events)
{
  // Valid events.
  EXPECT_TRUE(perf::valid({"cycles", "task-clock"}));

  // Invalid event among valid events.
  EXPECT_FALSE(perf::valid({"cycles", "task-clock", "invalid-event"}));
}


TEST_F(PerfTest, ROOT_PERF_Sample)
{
  // Sampling an empty set of cgroups should be a no-op.
  Future<hashmap<string, PerfStatistics>> sample =
    perf::sample({"cycles", "task-clock"}, {}, Seconds(1));

  AWAIT_READY(sample);

  EXPECT_TRUE(sample->empty());
}


TEST_F(PerfTest, Parse)
{
  // Parse multiple cgroups with uint64 and floats.
  Try<hashmap<string, mesos::PerfStatistics>> parse =
    perf::parse("123,cycles,cgroup1\n"
                "456,cycles,cgroup2\n"
                "0.456,task-clock,cgroup2\n"
                "0.123,task-clock,cgroup1",
                Version(3, 12, 0));

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
  parse = perf::parse("<not supported>,cycles,cgroup1", Version(3, 12, 0));
  ASSERT_SOME(parse);

  ASSERT_TRUE(parse->contains("cgroup1"));
  statistics = parse->get("cgroup1").get();
  EXPECT_FALSE(statistics.has_cycles());

  // Statistics reporting <not counted> should be zero.
  parse = perf::parse("<not counted>,cycles,cgroup1\n"
                      "<not counted>,task-clock,cgroup1",
                      Version(3, 12, 0));
  ASSERT_SOME(parse);

  ASSERT_TRUE(parse->contains("cgroup1"));
  statistics = parse->get("cgroup1").get();

  EXPECT_TRUE(statistics.has_cycles());
  EXPECT_EQ(0u, statistics.cycles());
  EXPECT_TRUE(statistics.has_task_clock());
  EXPECT_EQ(0.0, statistics.task_clock());

  // Check parsing fails.
  parse = perf::parse("1,cycles\ngarbage", Version(3, 12, 0));
  EXPECT_ERROR(parse);

  parse = perf::parse("1,unknown-field", Version(3, 12, 0));
  EXPECT_ERROR(parse);
}


// Test whether we can parse the perf version. Note that this avoids
// the "PERF_" filter to verify that we can parse the version even if
// the version check performed in the test filter fails.
TEST_F(PerfTest, Version)
{
  // If there is a "perf" command that can successfully emit its
  // version, make sure we can parse it using the perf library.
  // Note that on some systems, perf is a stub that asks you to
  // install the right packages.
  if (WSUCCEEDED(os::spawn("perf", {"perf", "--version"}))) {
    AWAIT_READY(perf::version());
  }

  EXPECT_SOME_EQ(Version(1, 0, 0), perf::parseVersion("1"));
  EXPECT_SOME_EQ(Version(1, 2, 0), perf::parseVersion("1.2"));
  EXPECT_SOME_EQ(Version(1, 2, 0), perf::parseVersion("1.2.3"));
  EXPECT_SOME_EQ(Version(0, 0, 0), perf::parseVersion("0.0.0"));

  // Fedora 25.
  EXPECT_SOME_EQ(
      Version(4, 8, 0),
      perf::parseVersion("4.8.16.300.fc25.x86_64.ge69a"));

  // Arch Linux.
  EXPECT_SOME_EQ(
      Version(4, 9, 0),
      perf::parseVersion("4.9.g69973b"));

  // CentOS 6.8.
  EXPECT_SOME_EQ(
      Version(2, 6, 0),
      perf::parseVersion("2.6.32-642.13.1.el6.x86_64.debug"));

  EXPECT_ERROR(perf::parseVersion(""));
  EXPECT_ERROR(perf::parseVersion("foo"));
  EXPECT_ERROR(perf::parseVersion("1.foo"));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
