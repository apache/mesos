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

#include <unistd.h> // For getpid, getppid.

#include <gmock/gmock.h>

#include <set>

#include <stout/try.hpp>

#include "linux/proc.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;

using proc::CPU;
using proc::SystemStatistics;
using proc::ProcessStatistics;

using std::set;


TEST(ProcTest, Pids)
{
  Try<set<pid_t> > pids = proc::pids();

  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));
}


TEST(ProcTest, Cpus)
{
  Try<std::list<CPU> > cpus = proc::cpus();

  ASSERT_SOME(cpus);
  EXPECT_LE(1u, cpus.get().size());
}


TEST(ProcTest, SystemStatistics)
{
  Try<SystemStatistics> statistics = proc::stat();

  ASSERT_SOME(statistics);
  EXPECT_NE(0u, statistics.get().btime);
}


TEST(ProcTest, ProcessStatistics)
{
  Try<ProcessStatistics> statistics = proc::stat(getpid());

  ASSERT_SOME(statistics);
  EXPECT_EQ(getpid(), statistics.get().pid);
  EXPECT_EQ(getppid(), statistics.get().ppid);
}
