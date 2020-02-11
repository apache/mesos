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
#include <gtest/gtest.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>

#include <stout/os/constants.hpp>

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/kill_policy_test_helper.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/utils.hpp"

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

using std::string;
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
      SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

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
       SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

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


// This test ensures that the command executor will terminate a task after it
// reaches `max_completion_time`.
TEST_P(CommandExecutorTest, MaxCompletionTime)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = GetParam();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Although the framework is started with the task killing capability,
  // it should not receive a `TASK_KILLING` status update.
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
       SLEEP_COMMAND(1000));

  // Set a `max_completion_time` for 2 seconds. Hopefully this should not
  // block test too long and still keep it reliable.
  task.mutable_max_completion_time()->set_nanoseconds(Seconds(2).ns());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFailed;

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  EXPECT_EQ(
      TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED, statusFailed->reason());

  driver.stop();
  driver.join();
}


// TODO(qianzhang): Kill policy helpers are not yet enabled on Windows. See
// MESOS-8168.
#ifndef __WINDOWS__
// This test ensures that a task will transition straight from `TASK_KILLING` to
// `TASK_KILLED`, even if the health check begins to fail during the kill policy
// grace period.
//
// TODO(gkleiman): this test takes about 7 seconds to run, consider using mock
// tasks and health checkers to speed it up.
TEST_P(CommandExecutorTest, NoTransitionFromKillingToRunning)
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

  const string command = strings::format(
      "%s %s --sleep_duration=15",
      getTestHelperPath("test-helper"),
      KillPolicyTestHelper::NAME).get();

  TaskInfo task = createTask(offers->front(), command);

  // Create a health check that succeeds until a temporary file is removed.
  Try<string> temporaryPath = os::mktemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(temporaryPath);
  const string tmpPath = temporaryPath.get();

  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value(
      "ls " + tmpPath + " > " + os::DEV_NULL);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_grace_period_seconds(0);
  healthCheck.set_interval_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  // Set the kill policy grace period to 5 seconds.
  KillPolicy killPolicy;
  killPolicy.mutable_grace_period()->set_nanoseconds(Seconds(5).ns());

  task.mutable_kill_policy()->CopyFrom(killPolicy);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusKilling;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers->front().id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());
  EXPECT_FALSE(statusKilling->has_healthy());

  // Remove the temporary file, so that the health check fails.
  os::rm(tmpPath);

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_FALSE(statusKilled->has_healthy());

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test verifies that the `MESOS_ALLOCATION_ROLE`
// environment variable is set properly.
TEST_P(CommandExecutorTest, AllocationRoleEnvironmentVariable)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = GetParam();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Start the framework with a role specified.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");

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
#ifdef __WINDOWS__
      "if %MESOS_ALLOCATION_ROLE% == \"role1\" (exit 1)");
#else
      "if [ \"$MESOS_ALLOCATION_ROLE\" != \"role1\" ]; then exit 1; fi");
#endif // __WINDOWS__

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


#ifndef __WINDOWS__
// This test checks that the command executor can communicate
// with the agent using unix domain sockets, when the necessary
// flags are set on the agent.
TEST_P(CommandExecutorTest, ExecutorDomainSockets)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = GetParam();
  flags.http_executor_domain_sockets = true;
  flags.domain_socket_location = *sandbox + "/agent.sock";

  // Forward `GLOG_v` environment variable to the executor;
  // useful when debugging tests.
  JSON::Object executorEnvironment;
  executorEnvironment.values["GLOG_v"] =
    JSON::String(os::getenv("GLOG_v").getOrElse("0"));

  flags.executor_environment_variables = executorEnvironment;

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
      "test -S $MESOS_DOMAIN_SOCKET");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // The task will be successful even if the executor is
  // not using domain sockets, because the container is
  // not mounting a separate root fs, so `$MESOS_DOMAIN_SOCKET`
  // will point to the agent domain socket on the host
  // filesystem.
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


class HTTPCommandExecutorTest
  : public MesosTest {};


// This test ensures that the HTTP command executor can self terminate
// after it gets the ACK for the terminal status update from agent.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HTTPCommandExecutorTest, TerminateWithACK)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.http_command_executor = true;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      containerizer.get(),
      flags,
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  Future<Future<Option<ContainerTermination>>> termination;
  EXPECT_CALL(*slave.get()->mock(), executorTerminated(_, _, _))
    .WillOnce(FutureArg<2>(&termination));

  driver.launchTasks(offers->front().id(), {task});

  // Scheduler should first receive TASK_STARTING, followed by TASK_RUNNING
  // and TASK_FINISHED.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // The executor should self terminate with 0 as exit status once
  // it gets the ACK for the terminal status update from agent.
  AWAIT_READY(termination);
  ASSERT_TRUE(termination->isReady());
  EXPECT_EQ(0, termination->get()->status());

  driver.stop();
  driver.join();
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
      SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Ensure no status update acknowledgements are sent from the driver
  // to the master until the explicit acknowledgement is sent.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _,
      master.get()->pid);

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_TRUE(statusStarting->has_slave_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  // Now send the acknowledgement.
  Future<mesos::scheduler::Call> acknowledgement = FUTURE_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _,
      master.get()->pid);

  driver.acknowledgeStatusUpdate(statusStarting.get());

  AWAIT_READY(acknowledgement);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
