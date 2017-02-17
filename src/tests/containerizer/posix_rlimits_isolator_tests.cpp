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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include "slave/flags.hpp"
#include "tests/cluster.hpp"
#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class PosixRLimitsIsolatorTest : public MesosTest {};


// This test checks the behavior of passed invalid limits.
TEST_F(PosixRLimitsIsolatorTest, InvalidLimits)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/rlimits";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      "true");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Set impossible limit soft > hard.
  RLimitInfo rlimitInfo;
  RLimitInfo::RLimit* rlimit = rlimitInfo.add_rlimits();
  rlimit->set_type(RLimitInfo::RLimit::RLMT_CPU);
  rlimit->set_soft(100);
  rlimit->set_hard(1);

  container->mutable_rlimit_info()->CopyFrom(rlimitInfo);

  Future<TaskStatus> taskStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&taskStatus));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(taskStatus);
  EXPECT_EQ(task.task_id(), taskStatus->task_id());
  EXPECT_EQ(TASK_FAILED, taskStatus->state());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, taskStatus->reason());

  driver.stop();
  driver.join();
}


// This test confirms that setting no values for the soft and hard
// limits implies an unlimited resource.
TEST_F(PosixRLimitsIsolatorTest, UnsetLimits) {
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/rlimits";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      "exit `ulimit -c | grep -q unlimited`");

  // Force usage of C locale as we interpret a potentially translated
  // string in the task's command.
  mesos::Environment::Variable* locale =
      task.mutable_command()->mutable_environment()->add_variables();
  locale->set_name("LC_ALL");
  locale->set_value("C");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Setting rlimit for core without soft or hard limit signifies
  // unlimited range.
  RLimitInfo rlimitInfo;
  RLimitInfo::RLimit* rlimit = rlimitInfo.add_rlimits();
  rlimit->set_type(RLimitInfo::RLimit::RLMT_CORE);

  container->mutable_rlimit_info()->CopyFrom(rlimitInfo);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinal;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&statusRunning))
      .WillOnce(FutureArg<1>(&statusFinal));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinal);
  EXPECT_EQ(task.task_id(), statusFinal->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinal->state());

  driver.stop();
  driver.join();
}


// This test confirms that setting just one of the soft/hard limits is
// an error.
TEST_F(PosixRLimitsIsolatorTest, BothSoftAndHardLimitSet)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/rlimits";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      "true");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  RLimitInfo rlimitInfo;
  RLimitInfo::RLimit* rlimit = rlimitInfo.add_rlimits();
  rlimit->set_type(RLimitInfo::RLimit::RLMT_CORE);
  rlimit->set_soft(1);

  container->mutable_rlimit_info()->CopyFrom(rlimitInfo);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, status->reason());

  driver.stop();
  driver.join();
}


// This test confirms that if a task exceeds configured resource
// limits it is forcibly terminated.
TEST_F(PosixRLimitsIsolatorTest, TaskExceedingLimit)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/rlimits";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  // The task attempts to use an infinite amount of CPU time.
  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      "while true; do true; done");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Limit the process to use maximally 1 second of CPU time.
  RLimitInfo rlimitInfo;
  RLimitInfo::RLimit* cpuLimit = rlimitInfo.add_rlimits();
  cpuLimit->set_type(RLimitInfo::RLimit::RLMT_CPU);
  cpuLimit->set_soft(1);
  cpuLimit->set_hard(1);

  container->mutable_rlimit_info()->CopyFrom(rlimitInfo);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFailed);
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
