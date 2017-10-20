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

#include <string>
#include <vector>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using std::string;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class MemoryIsolatorTest
  : public MesosTest,
    public WithParamInterface<string> {};


// These tests are parameterized by the isolation flag.
static vector<string>* isolators = new vector<string>({
#ifndef __WINDOWS__
  "posix/mem",
#ifdef __linux__
  "cgroups/mem",
#endif // __linux__
#else
  "windows/mem",
#endif // __WINDOWS__
});


INSTANTIATE_TEST_CASE_P(
    IsolationFlag,
    MemoryIsolatorTest,
    ::testing::ValuesIn(*isolators));


TEST_P(MemoryIsolatorTest, ROOT_MemUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = GetParam();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(120));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // TODO(jieyu): Consider using a program that predictably increases
  // RSS so that we can set more meaningful expectation here.
#ifdef __WINDOWS__
  // NOTE: Windows reports `mem_total_bytes` instead of `mem_rss_bytes` because
  // of the change in meaning in 0.23.0. The `mem_rss_bytes` represents only
  // anonymous memory usage, but the "working set size" reported by Windows
  // corresponds to the total memory of a process in RAM.
  EXPECT_LT(0u, usage->mem_total_bytes());
#else
  // TODO(andschwa): Consider testing `mem_total_bytes` for other platforms.
  EXPECT_LT(0u, usage->mem_rss_bytes());
#endif // __WINDOWS__

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
