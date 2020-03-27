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
#include <stout/option.hpp>
#include <stout/stringify.hpp>

#include <stout/os/exec.hpp>

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
    perf::parse(
        "123,cycles,cgroup1\n"
        "456,cycles,cgroup2\n"
        "0.456,task-clock,cgroup2\n"
        "0.123,task-clock,cgroup1\n"
        "5812843447,,cycles,cgroup3,3560494814,100.00,0.097,GHz\n"
        "60011.034108,,task-clock,cgroup3,60011034108,100.00,11.999,CPUs utilized"); // NOLINT(whitespace/line_length)

  ASSERT_SOME(parse);
  EXPECT_EQ(3u, parse->size());

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

  // Due to a bug 'perf stat' may append extra CSV separators. Check
  // that we handle this situation correctly.
  parse = perf::parse(
      "<not supported>,,stalled-cycles-backend,cgroup1,0,100.00,,,,");
  ASSERT_SOME(parse);
  ASSERT_TRUE(parse->contains("cgroup1"));

  statistics = parse->get("cgroup1").get();
  EXPECT_FALSE(statistics.has_stalled_cycles_backend());

  // Some additional metrics (e.g. stalled cycles per instruction) can
  // be printed without an event. They should be ignored.
  parse = perf::parse(
      "11651954,,instructions,cgroup1,1041690,10.63,0.58,insn per cycle\n"
      ",,,,,,1.29,stalled cycles per insn\n"
      "14995512,,stalled-cycles-frontend,cgroup1,1464204,14.94,75.26,frontend cycles idle"); // NOLINT(whitespace/line_length)
  ASSERT_SOME(parse);
  ASSERT_TRUE(parse->contains("cgroup1"));

  statistics = parse->get("cgroup1").get();
  EXPECT_TRUE(statistics.has_instructions());
  EXPECT_EQ(11651954u, statistics.instructions());
  EXPECT_TRUE(statistics.has_stalled_cycles_frontend());
  EXPECT_EQ(14995512u, statistics.stalled_cycles_frontend());

  // Check parsing fails.
  parse = perf::parse("1,cycles\ngarbage");
  EXPECT_ERROR(parse);

  parse = perf::parse("1,unknown-field");
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
  const Option<int> status = os::spawn("perf", {"perf", "--version"});
  if (status.isSome() && WSUCCEEDED(status.get())) {
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
