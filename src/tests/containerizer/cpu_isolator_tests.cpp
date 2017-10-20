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

class CpuIsolatorTest
  : public MesosTest,
    public WithParamInterface<string> {};


// These tests are parameterized by the isolation flag.
static vector<string>* isolators = new vector<string>({
#ifndef __WINDOWS__
  "posix/cpu",
#ifdef __linux__
  "cgroups/cpu",
#endif // __linux__
#else
  "windows/cpu",
#endif // __WINDOWS__
});


INSTANTIATE_TEST_CASE_P(
    IsolationFlag,
    CpuIsolatorTest,
    ::testing::ValuesIn(*isolators));


TEST_P(CpuIsolatorTest, ROOT_UserCpuUsage)
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

  // Max out a single core in userspace. This will run for at most one
  // second.
  TaskInfo task = createTask(
      offers.get()[0],
#ifdef __WINDOWS__
      "powershell -c \"while ($true) {}\""
#else
      "while true ; do true ; done & sleep 60"
#endif // __WINDOWS__
    );

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

  // Wait up to 1 second for the child process to induce 1/8 of a
  // second of user cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.cpus_user_time_secs() >= 0.125) {
      break;
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(1));

  EXPECT_LE(0.125, statistics.cpus_user_time_secs());

  driver.stop();
  driver.join();
}


// TODO(andschwa): Enable this test when a command can be found that does not
// cause a flaky test. As it is, it is difficult to consume kernel time on
// Windows consistently.
TEST_P_TEMP_DISABLED_ON_WINDOWS(CpuIsolatorTest, ROOT_SystemCpuUsage)
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

  TaskInfo task = createTask(
      offers.get()[0],
#ifdef __WINDOWS__
      // Enumerating processes will at least cause some kernel time.
      "powershell -NoProfile -Command "
      "\"while ($true) { Get-Process | Out-Null }\""
#else
      // Generating random numbers is done by the kernel and will max out
      // a single core and run almost exclusively in the kernel, i.e.,
      // system time.
      "cat /dev/urandom > /dev/null & sleep 60"
#endif // __WINDOWS__
    );

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

  // Wait up to 1 seconds for the child process to induce 1/8 of a
  // second of system cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.cpus_system_time_secs() >= 0.125) {
      break;
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(1));

  EXPECT_LE(0.125, statistics.cpus_system_time_secs());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
