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

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;
using process::PID;

using std::vector;

using ::testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// Tests that exercise the command executor implementation
// should be located in this file.

class CommandExecutorTest
  : public MesosTest,
    public WithParamInterface<bool> {};


// The Command Executor tests are parameterized by the underlying library
// used by the executor (e.g., driver or the HTTP based executor library).
INSTANTIATE_TEST_CASE_P(
    HTTPCommandExecutor,
    CommandExecutorTest,
    ::testing::Bool());


// This test ensures that the command executor does not send
// TASK_KILLING to frameworks that do not support the capability.
TEST_P(CommandExecutorTest, NoTaskKillingCapability)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = GetParam();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Start the framework without the task killing capability.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "sleep 1000");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // There should only be a TASK_KILLED update.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  driver.stop();
  driver.join();
}


// This test ensures that the command executor sends TASK_KILLING
// to frameworks that support the capability.
TEST_P(CommandExecutorTest, TaskKillingCapability)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = GetParam();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Start the framework with the task killing capability.
  FrameworkInfo::Capability capability;
  capability.set_type(FrameworkInfo::Capability::TASK_KILLING_STATE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
       "sleep 1000");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<TaskStatus> statusKilling, statusKilled;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  driver.stop();
  driver.join();
}


class HTTPCommandExecutorTest
  : public MesosTest {};


// This test ensures that the HTTP command executor can self terminate
// after it gets the ACK for the terminal status update from agent.
TEST_F(HTTPCommandExecutorTest, TerminateWithACK)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  CHECK_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave(flags, &detector, containerizer.get());
  spawn(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Launch a short lived task.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "sleep 1");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  Future<Future<Option<ContainerTermination>>> termination;
  EXPECT_CALL(slave, executorTerminated(_, _, _))
    .WillOnce(FutureArg<2>(&termination));

  driver.launchTasks(offers->front().id(), {task});

  // Scheduler should first receive TASK_RUNNING followed by TASK_FINISHED.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // The executor should self terminate with 0 as exit status once
  // it gets the ACK for the terminal status update from agent.
  AWAIT_READY(termination);
  ASSERT_TRUE(termination.get().isReady());
  EXPECT_EQ(0, termination.get().get().get().status());

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);
}


// This test ensures that driver based schedulers using explicit
// acknowledgements can acknowledge status updates sent from
// HTTP based executors.
TEST_F(HTTPCommandExecutorTest, ExplicitAcknowledgements)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = true;

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "sleep 1000");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  // Ensure no status update acknowledgements are sent from the driver
  // to the master until the explicit acknowledgement is sent.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _ ,
      master.get()->pid);

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_TRUE(statusRunning->has_slave_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Now send the acknowledgement.
  Future<mesos::scheduler::Call> acknowledgement = FUTURE_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _,
      master.get()->pid);

  driver.acknowledgeStatusUpdate(statusRunning.get());

  AWAIT_READY(acknowledgement);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
