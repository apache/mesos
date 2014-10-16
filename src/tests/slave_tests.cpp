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

#include <unistd.h>

#include <gmock/gmock.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/graceful_shutdown.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::tests;

using namespace process;

using mesos::master::Master;

using mesos::slave::Containerizer;
using mesos::slave::Fetcher;
using mesos::slave::GarbageCollectorProcess;
using mesos::slave::MesosContainerizer;
using mesos::slave::MesosContainerizerProcess;
using mesos::slave::Slave;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::Return;
using testing::SaveArg;

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more slave than master-related are in this file.
// The others are in "master_tests.cpp".

class SlaveTest : public MesosTest {};


TEST_F(SlaveTest, ShutdownUnregisteredExecutor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  // Set the isolation flag so we know a MesoContainerizer will be created.
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_value("sleep 10");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Drop the registration message from the executor to the slave.
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(registerExecutor);

  Clock::pause();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure that the slave times out and kills the executor.
  Future<Nothing> destroyExecutor =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::destroy);

  Clock::advance(flags.executor_registration_timeout);

  AWAIT_READY(destroyExecutor);

  Clock::settle(); // Wait for Containerizer::destroy to complete.

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status.get().state());

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that when an executor terminates before
// registering with slave, it is properly cleaned up.
TEST_F(SlaveTest, RemoveUnregisteredTerminatedExecutor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Drop the registration message from the executor to the slave.
  Future<Message> registerExecutorMessage =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(registerExecutorMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  // Now kill the executor.
  containerizer.destroy(offers.get()[0].framework_id(), DEFAULT_EXECUTOR_ID);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // We use 'gc.schedule' as a signal for the executor being cleaned
  // up by the slave.
  AWAIT_READY(schedule);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test that we can run the command executor and specify an "override"
// command to use via the --override argument.
TEST_F(SlaveTest, CommandExecutorWithOverride)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_value("sleep 10");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Expect the launch and just assume it was sucessful since we'll be
  // launching the executor ourselves manually below.
  Future<Nothing> launch;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&launch),
                    Return(true)));

  // Expect wait after launch is called but don't return anything
  // until after we've finished everything below.
  Future<Nothing> wait;
  Promise<containerizer::Termination> promise;
  EXPECT_CALL(containerizer, wait(_))
    .WillOnce(DoAll(FutureSatisfy(&wait),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Once we get the launch the mesos-executor with --override.
  AWAIT_READY(launch);

  // Set up fake environment for executor.
  map<string, string> environment;
  environment["MESOS_SLAVE_PID"] = stringify(slave.get());
  environment["MESOS_SLAVE_ID"] = stringify(offers.get()[0].slave_id());
  environment["MESOS_FRAMEWORK_ID"] = stringify(offers.get()[0].framework_id());
  environment["MESOS_EXECUTOR_ID"] = stringify(task.task_id());
  environment["MESOS_DIRECTORY"] = "";

  // Create temporary file to store validation string. If command is
  // succesfully replaced, this file will end up containing the string
  // 'Hello World\n'. Otherwise, the original task command i.e.
  // 'sleep' will be called and the test will fail.
  Try<std::string> file = os::mktemp();
  ASSERT_SOME(file);

  string executorCommand =
    path::join(tests::flags.build_dir, "src", "mesos-executor") +
    " --override -- /bin/sh -c 'echo hello world >" + file.get() + "'";

  // Expect two status updates, one for once the mesos-executor says
  // the task is running and one for after our overridden command
  // above finishes.
  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  Try<Subprocess> executor =
    subprocess(
        executorCommand,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        environment);

  ASSERT_SOME(executor);

  // Scheduler should receive the TASK_RUNNING update.
  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_FINISHED, status2.get().state());

  AWAIT_READY(wait);

  containerizer::Termination termination;
  termination.set_killed(false);
  termination.set_message("Killed executor");
  termination.set_status(0);
  promise.set(termination);

  driver.stop();
  driver.join();

  AWAIT_READY(executor.get().status());

  // Verify file contents.
  Try<std::string> validate = os::read(file.get());
  ASSERT_SOME(validate);

  EXPECT_EQ(validate.get(), "hello world\n");

  os::rm(file.get());

  Shutdown();
}


// Test that we don't let task arguments bleed over as
// mesos-executor args. For more details of this see MESOS-1873.
//
// This assumes the ability to execute '/bin/echo --author'.
TEST_F(SlaveTest, ComamndTaskWithArguments)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Don't let args from the CommandInfo struct bleed over into
// mesos-executor forking. For more details of this see MESOS-1873.
TEST_F(SlaveTest, GetExecutorInfo)
{
  // Create a thin dummy Slave to access underlying getExecutorInfo().
  // Testing this method should not necessarily require an integration
  // test as with most other methods here.
  slave::Flags flags = CreateSlaveFlags();
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;
  Files files;
  slave::StatusUpdateManager updateManager(flags);

  slave::GarbageCollector gc;
  Slave slave(flags, &detector, &containerizer, &files, &gc, &updateManager);

  FrameworkID frameworkId;
  frameworkId.set_value("20141010-221431-251662764-60288-32120-0000");

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-32120-0001");
  task.mutable_resources()->MergeFrom(
      Resources::parse("cpus:0.1;mem:32").get());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  const ExecutorInfo& executor = slave.getExecutorInfo(frameworkId, task);

  // Now assert that it actually is running mesos-executor without any
  // bleedover from the command we intend on running.
  EXPECT_TRUE(executor.command().shell());
  EXPECT_FALSE(executor.command().has_container());
  EXPECT_EQ(0, executor.command().arguments_size());
  EXPECT_NE(string::npos, executor.command().value().find("mesos-executor"));
}

// This test runs a command without the command user field set. The
// command will verify the assumption that the command is run as the
// slave user (in this case, root).
TEST_F(SlaveTest, ROOT_RunTaskWithCommandInfoWithoutUser)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Result<string> user = os::user();
  CHECK_SOME(user) << "Failed to get current user name"
                   << (user.isError() ? ": " + user.error() : "");

  const string helper =
      path::join(tests::flags.build_dir, "src", "active-user-test-helper");

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(false);
  command.set_value(helper);
  command.add_arguments(helper);
  command.add_arguments(user.get());

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test runs a command _with_ the command user field set. The
// command will verify the assumption that the command is run as the
// specified user. We use (and assume the precense) of the
// unprivileged 'nobody' user which should be available on both Linux
// and Mac OS X.
TEST_F(SlaveTest, ROOT_RunTaskWithCommandInfoWithUser)
{
  // TODO(nnielsen): Introduce STOUT abstraction for user verification
  // instead of flat getpwnam call.
  const string testUser = "nobody";
  if (::getpwnam(testUser.c_str()) == NULL) {
    LOG(WARNING) << "Cannot run ROOT_RunTaskWithCommandInfoWithUser test:"
                 << " user '" << testUser << "' is not present";
    return;
  }

  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  const string helper =
      path::join(tests::flags.build_dir, "src", "active-user-test-helper");

  CommandInfo command;
  command.set_user(testUser);
  command.set_shell(false);
  command.set_value(helper);
  command.add_arguments(helper);
  command.add_arguments(testUser);

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that a status update acknowledgement from a
// non-leading master is ignored.
TEST_F(SlaveTest, IgnoreNonLeaderStatusUpdateAcknowledgement)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // We need to grab this message to get the scheduler's pid.
  Future<Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  schedDriver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  const UPID schedulerPid = frameworkRegisteredMessage.get().to;

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&update));

  // Pause the clock to prevent status update retries on the slave.
  Clock::pause();

  // Intercept the acknowledgement sent to the slave so that we can
  // spoof the master's pid.
  Future<StatusUpdateAcknowledgementMessage> acknowledgementMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(),
                  master.get(),
                  slave.get());

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get(), &Slave::_statusUpdateAcknowledgement);

  schedDriver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update.get().state());

  AWAIT_READY(acknowledgementMessage);

  // Send the acknowledgement to the slave with a non-leading master.
  post(process::UPID("master@localhost:1"),
       slave.get(),
       acknowledgementMessage.get());

  // Make sure the acknowledgement was ignored.
  Clock::settle();
  ASSERT_TRUE(_statusUpdateAcknowledgement.isPending());

  // Make sure the status update gets retried because the slave
  // ignored the acknowledgement.
  Future<TaskStatus> retriedUpdate;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&retriedUpdate));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(retriedUpdate);

  // Ensure the slave receives and properly handles the ACK.
  // Clock::settle() ensures that the slave successfully
  // executes Slave::_statusUpdateAcknowledgement().
  AWAIT_READY(_statusUpdateAcknowledgement);
  Clock::settle();

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();

  Shutdown();
}


TEST_F(SlaveTest, MetricsInStatsEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  Future<http::Response> response =
    http::get(slave.get(), "stats.json");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, stats.values.count("slave/registered"));

  EXPECT_EQ(1u, stats.values.count("slave/recovery_errors"));

  EXPECT_EQ(1u, stats.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, stats.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_lost"));

  // TODO(dhamon): Add expectations for task source reason metrics.

  EXPECT_EQ(1u, stats.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_running"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminated"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(1u, stats.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/mem_total"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_used"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/disk_total"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_used"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_percent"));

  Shutdown();
}


TEST_F(SlaveTest, StateEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  flags.resources = "cpus:4;mem:2048;disk:512;ports:[33000-34000]";
  flags.attributes = "rack:abc;host:myhost";

  Try<PID<Slave> > slave = StartSlave(flags);
  ASSERT_SOME(slave);

  Future<http::Response> response =
    http::get(slave.get(), "state.json");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  // Check if 'resources' matches.
  Try<Resources> resources = Resources::parse(
      flags.resources.get(), flags.default_role);

  ASSERT_SOME(resources);

  ASSERT_EQ(1u, state.values.count("resources"));
  EXPECT_EQ(state.values["resources"], JSON::Value(model(resources.get())));

  // Check if 'attributes' matches.
  Attributes attributes = Attributes::parse(flags.attributes.get());

  ASSERT_EQ(1u, state.values.count("attributes"));
  EXPECT_EQ(state.values["attributes"], JSON::Value(model(attributes)));

  Shutdown();
}


// This test ensures that when a slave is shutting down, it will not
// try to re-register with the master.
TEST_F(SlaveTest, TerminatingSlaveDoesNotReregister)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Create a MockExecutor to enable us to catch
  // ShutdownExecutorMessage later.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Create a StandaloneMasterDetector to enable the slave to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get());
  slave::Flags flags = CreateSlaveFlags();

  // Make the executor_shutdown_grace_period to be much longer than
  // REGISTER_RETRY_INTERVAL, so that the slave will at least call
  // call doReliableRegistration() once before the slave is actually
  // terminated.
  flags.executor_shutdown_grace_period = slave::REGISTER_RETRY_INTERVAL_MAX * 2;

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave(&exec, &detector, flags);
  ASSERT_SOME(slave);

  // Create a task on the slave.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  // Launch a task that uses less resource than the
  // default(cpus:2, mem:1024).
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Pause the clock here so that after detecting a new master,
  // the slave will not send multiple reregister messages
  // before we change its state to TERMINATING.
  Clock::pause();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    DROP_PROTOBUF(SlaveReregisteredMessage(), master.get(), slave.get());

  // Simulate a new master detected event on the slave,
  // so that the slave will do a re-registration.
  detector.appoint(master.get());

  // Make sure the slave has entered doReliableRegistration()
  // before we change the slave's state.
  AWAIT_READY(slaveReregisteredMessage);

  // Setup an expectation that the master should not receive any
  // ReregisterSlaveMessage in the future.
  EXPECT_NO_FUTURE_PROTOBUFS(
      ReregisterSlaveMessage(), slave.get(), master.get());

  // Drop the ShutdownExecutorMessage, so that the slave will
  // stay in TERMINATING for a while.
  DROP_PROTOBUFS(ShutdownExecutorMessage(), slave.get(), _);

  // Send a ShutdownMessage instead of calling Stop() directly
  // to avoid blocking.
  post(master.get(), slave.get(), ShutdownMessage());

  // Advance the clock to trigger doReliableRegistration().
  Clock::advance(slave::REGISTER_RETRY_INTERVAL_MAX * 2);
  Clock::settle();
  Clock::resume();

  // Clean up.
  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies the slave will destroy a container if, when
// receiving a terminal status task update, updating the container's
// resources fails.
TEST_F(SlaveTest, TerminalTaskContainerizerUpdateFails)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  EXPECT_CALL(exec, registered(_, _, _, _));

  TestContainerizer containerizer(&exec);

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Offer offer = offers.get()[0];

  // Start two tasks.
  vector<TaskInfo> tasks;

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      "sleep 1000",
      exec.id));

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      "sleep 1000",
      exec.id));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1, status2, status3, status4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  // Set up the containerizer so the next update() will fail.
  EXPECT_CALL(containerizer, update(_, _))
    .WillOnce(Return(Failure("update() failed")))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  // Kill one of the tasks. The failed update should result in the
  // second task going lost when the container is destroyed.
  driver.killTask(tasks[0].task_id());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_KILLED, status3.get().state());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_LOST, status4.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that the slave will re-register with the master
// if it does not receive any pings after registering.
TEST_F(SlaveTest, PingTimeoutNoPings)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Block all pings to the slave.
  DROP_MESSAGES(Eq("PING"), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Ensure the slave processes the registration message and schedules
  // the ping timeout, before we advance the clock.
  Clock::pause();
  Clock::settle();

  // Advance to the ping timeout to trigger a re-detection and
  // re-registration.
  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(slave::MASTER_PING_TIMEOUT());

  AWAIT_READY(detected);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that the slave will re-register with the master
// if it stops receiving pings.
TEST_F(SlaveTest, PingTimeoutSomePings)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  Clock::pause();

  // Ensure a ping reaches the slave.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  Clock::advance(master::SLAVE_PING_TIMEOUT);

  AWAIT_READY(ping);

  // Now block further pings from the master and advance
  // the clock to trigger a re-detection and re-registration on
  // the slave.
  DROP_MESSAGES(Eq("PING"), _, _);

  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(slave::MASTER_PING_TIMEOUT());

  AWAIT_READY(detected);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that when slave removal rate limit is specified
// a slave that fails health checks is removed after acquiring permit
// from the rate limiter.
TEST_F(SlaveTest, RateLimitSlaveShutdown)
{
  // Start a master.
  master::Flags flags = CreateMasterFlags();
  flags.slave_removal_rate_limit = "1/1secs";
  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_MESSAGES(Eq("PONG"), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  Future<Nothing> acquire =
    FUTURE_DISPATCH(_, &RateLimiterProcess::acquire);

  Future<ShutdownMessage> shutdown = FUTURE_PROTOBUF(ShutdownMessage(), _, _);

  // Now advance through the PINGs.
  Clock::pause();
  uint32_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT);
  Clock::settle();

  // Master should acquire the permit for shutting down the slave.
  AWAIT_READY(acquire);

  // Master should shutdown the slave.
  AWAIT_READY(shutdown);
}


// This test verifies that when a slave responds to pings after the
// slave observer has scheduled it for shutdown (due to health check
// failure), the shutdown is cancelled.
TEST_F(SlaveTest, CancelSlaveShutdown)
{
  // Start a master.
  master::Flags flags = CreateMasterFlags();
  // Interval between slave removals.
  Duration interval = master::SLAVE_PING_TIMEOUT * 10;
  flags.slave_removal_rate_limit = "1/" + stringify(interval);
  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_MESSAGES(Eq("PONG"), _, _);

  // NOTE: We start two slaves in this test so that the rate limiter
  // used by the slave observers gives out 2 permits. The first permit
  // gets satisfied immediately. And since the 2nd permit will be
  // enqueued, it's corresponding future can be discarded before it
  // becomes ready.
  // TODO(vinod): Inject a rate limiter into 'Master' instead to
  // simplify the test.

  // Start the first slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<PID<Slave> > slave1 = StartSlave();
  ASSERT_SOME(slave1);

  AWAIT_READY(slaveRegisteredMessage1);

  // Start the second slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<PID<Slave> > slave2 = StartSlave();
  ASSERT_SOME(slave2);

  AWAIT_READY(slaveRegisteredMessage2);

  Future<Nothing> acquire1 =
    FUTURE_DISPATCH(_, &RateLimiterProcess::acquire);

  Future<Nothing> acquire2 =
    FUTURE_DISPATCH(_, &RateLimiterProcess::acquire);

  Future<ShutdownMessage> shutdown = FUTURE_PROTOBUF(ShutdownMessage(), _, _);

  // Now advance through the PINGs until shutdown permits are given
  // out for both the slaves.
  Clock::pause();
  while (true) {
    AWAIT_READY(ping);
    if (acquire1.isReady() && acquire2.isReady()) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
  }
  Clock::settle();

  // The slave that first timed out should be shutdown right away.
  AWAIT_READY(shutdown);

  // Ensure the 2nd slave's shutdown is canceled.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  // Reset the filters to allow pongs from the 2nd slave.
  filter(NULL);

  // Advance clock enough to do a ping pong.
  Clock::advance(master::SLAVE_PING_TIMEOUT);
  Clock::settle();

  // Now advance the clock to the time the 2nd permit is acquired.
  Clock::advance(interval);

  // Settle the clock to give a chance for the master to shutdown
  // the 2nd slave (it shouldn't in this test).
  Clock::settle();
}


// This test ensures that a killTask() can happen between runTask()
// and _runTask() and then gets "handled properly". This means that
// the task never gets started, but also does not get lost. The end
// result is status TASK_KILLED. Essentially, killing the task is
// realized while preparing to start it. See MESOS-947. This test
// removes the framework and proves that removeFramework() is
// called. See MESOS-1945.
TEST_F(SlaveTest, KillTaskBetweenRunTaskParts)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get());

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillRepeatedly(FutureArg<1>(&status));

  EXPECT_CALL(slave, runTask(_, _, _, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_runTask));

  // Saved arguments from Slave::_runTask().
  Future<bool> future;
  FrameworkInfo frameworkInfo;
  FrameworkID frameworkId;

  // Skip what Slave::_runTask() normally does, save its arguments for
  // later, tie reaching the critical moment when to kill the task to
  // a future.
  Future<Nothing> _runTask;
  EXPECT_CALL(slave, _runTask(_, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_runTask),
                    SaveArg<0>(&future),
                    SaveArg<1>(&frameworkInfo),
                    SaveArg<2>(&frameworkId)));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(_runTask);

  // Since this is the only task ever for this framework, the
  // framework should get removed in Slave::killTask().
  // Thus we can observe that this happens before Shutdown().
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  Future<Nothing> killTask;
  EXPECT_CALL(slave, killTask(_, _, _))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  driver.killTask(task.task_id());

  AWAIT_READY(killTask);
  slave.unmocked__runTask(
      future, frameworkInfo, frameworkId, master.get(), task);

  AWAIT_READY(removeFramework);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that when a slave re-registers with the master
// it correctly includes the latest and status update task states.
TEST_F(SlaveTest, ReregisterWithStatusUpdateTaskState)
{
  // Start a master.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Create a StandaloneMasterDetector to enable the slave to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get());

  // Start a slave.
  Try<PID<Slave> > slave = StartSlave(&exec, &detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 1024, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Signal when the first update is dropped.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, master.get());

  Future<Nothing> __statusUpdate = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  driver.start();

  // Pause the clock to avoid status update retries.
  Clock::pause();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure status update manager handles TASK_RUNNING update.
  AWAIT_READY(__statusUpdate);

  Future<Nothing> __statusUpdate2 = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage.get().update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure status update manager handles TASK_FINISHED update.
  AWAIT_READY(__statusUpdate2);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Drop any updates to the failed over master.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get());

  // Simulate a new master detected event on the slave,
  // so that the slave will do a re-registration.
  detector.appoint(master.get());

  // Capture and inspect the slave reregistration message.
  AWAIT_READY(reregisterSlaveMessage);

  ASSERT_EQ(1, reregisterSlaveMessage.get().tasks_size());

  // The latest state of the task should be TASK_FINISHED.
  ASSERT_EQ(TASK_FINISHED, reregisterSlaveMessage.get().tasks(0).state());

  // The status update state of the task should be TASK_RUNNING.
  ASSERT_EQ(TASK_RUNNING,
            reregisterSlaveMessage.get().tasks(0).status_update_state());

  // The status update uuid should match the TASK_RUNNING's uuid.
  ASSERT_EQ(statusUpdateMessage.get().update().uuid(),
            reregisterSlaveMessage.get().tasks(0).status_update_uuid());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that label values can be set for tasks and that
// they are exposed over the slave state endpoint.
TEST_F(SlaveTest, TaskLabels)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = task.mutable_labels();

  Label* label1 = labels->add_labels();
  label1->set_key("foo");
  label1->set_value("bar");

  Label* label2 = labels->add_labels();
  label2->set_key("bar");
  label2->set_value("baz");

  Label* label3 = labels->add_labels();
  label3->set_key("bar");
  label3->set_value("qux");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Future<Nothing>())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in slave state.json.
  Future<http::Response> response =
    http::get(slave.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labelsObject = parse.get().find<JSON::Array>(
      "frameworks[0].executors[0].tasks[0].labels");
  EXPECT_SOME(labelsObject);

  JSON::Array labelsObject_ = labelsObject.get();

  // Verify the content of 'foo:bar' pair.
  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"key\":\"foo\","
      "  \"value\":\"bar\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[0], expected.get());


  // Verify the content of 'bar:baz' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"bar\","
      "  \"value\":\"baz\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[1], expected.get());


  // Verify the content of 'bar:qux' pair.
  expected = JSON::parse(
      "{"
      "  \"key\":\"bar\","
      "  \"value\":\"qux\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(labelsObject_.values[2], expected.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test checks that the mechanism of calculating nested graceful
// shutdown periods does not break the default behaviour and works as
// expected.
TEST_F(SlaveTest, ShutdownGracePeriod)
{
  Duration defaultTimeout = slave::EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  Duration customTimeout = Seconds(10);

  // We used to have a signal escalation timeout constant responsibe
  // for graceful shutdown period in the CommandExecutor. Make sure
  // the default behaviour (3s) persists.
  EXPECT_EQ(Seconds(3), slave::getExecutorGracePeriod(defaultTimeout));

  // The new logic uses a certain delta to calculate nested timeouts.
  EXPECT_EQ(Duration::zero(), slave::getExecutorGracePeriod(Duration::zero()));
  EXPECT_EQ(Seconds(2), slave::getContainerizerGracePeriod(Duration::zero()));
  EXPECT_EQ(customTimeout + Seconds(2),
            slave::getContainerizerGracePeriod(customTimeout));

  // The grace period in ExecutorProcess should be bigger than the
  // grace period in an executor.
  EXPECT_GT(slave::getExecGracePeriod(defaultTimeout),
            slave::getExecutorGracePeriod(defaultTimeout));

  // Check the graceful shutdown periods that reach the executor in
  // protobuf messages.
  // NOTE: We check only the message contents and *not* the value
  // stored by the executor.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.executor_shutdown_grace_period = slave::EXECUTOR_SHUTDOWN_GRACE_PERIOD;

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));
  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());
  Offer offer = offers.get()[0];

  // Create one task with shutdown grace period set and one without.
  TaskInfo taskCustom;
  taskCustom.set_name("Task with custom grace shutdown period");
  taskCustom.mutable_task_id()->set_value("custom");
  taskCustom.mutable_slave_id()->MergeFrom(offer.slave_id());
  taskCustom.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  taskCustom.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.1;mem:64").get());
  taskCustom.mutable_executor()->mutable_command()->set_grace_period_seconds(
      Seconds(customTimeout).value());

  TaskInfo taskDefault;
  taskDefault.set_name("Task with default grace shutdown period");
  taskDefault.mutable_task_id()->set_value("default");
  taskDefault.mutable_slave_id()->MergeFrom(offer.slave_id());
  taskDefault.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  taskDefault.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.1;mem:64").get());

  ASSERT_TRUE(Resources(offer.resources()).contains(
      taskCustom.resources() + taskDefault.resources()));

  vector<TaskInfo> tasks;
  tasks.push_back(taskCustom);
  tasks.push_back(taskDefault);

  Future<TaskInfo> task1, task2;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&task1))
    .WillOnce(FutureArg<1>(&task2));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(task1);
  AWAIT_READY(task2);

  // Currently (14 Nov 2014) grace periods customized by frameworks
  // are ignored.
  EXPECT_DOUBLE_EQ(
      Seconds(defaultTimeout).value(),
      task1.get().executor().command().grace_period_seconds());
  EXPECT_DOUBLE_EQ
      (Seconds(defaultTimeout).value(),
      task2.get().executor().command().grace_period_seconds());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that the graceful shutdown in the command
// uses SIGTERM to gracefully shutdown a task.
TEST_F(SlaveTest, CommandExecutorGracefulShutdown)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Explicitly set the grace period for the executor.
  // NOTE: We ensure that the graceful shutdown is at least
  // 10 seconds because we've observed the sleep command to take
  // several seconds to terminate from SIGTERM on some slow CI VMs.
  slave::Flags flags = CreateSlaveFlags();
  flags.executor_shutdown_grace_period = std::max(
      slave::EXECUTOR_SHUTDOWN_GRACE_PERIOD,
      Duration(Seconds(10)));

  // Ensure that a reap will occur within the grace period.
  Duration timeout = slave::getExecutorGracePeriod(
      flags.executor_shutdown_grace_period);
  EXPECT_GT(timeout, MAX_REAP_INTERVAL());

  Fetcher fetcher;
  Try<MesosContainerizer*> containerizer = MesosContainerizer::create(
      flags, true, &fetcher);
  ASSERT_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());
  Offer offer = offers.get()[0];

  // Launch a long-running task responsive to SIGTERM.
  TaskInfo taskResponsive = createTask(offer, "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(taskResponsive);

  Future<TaskStatus> statusRunning, statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  driver.killTask(taskResponsive.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());

  // CommandExecutor supports graceful shutdown in sending SIGTERM
  // first. If the task obeys, it will be reaped and we get
  // appropriate status message.
  // NOTE: strsignal() behaves differently on Mac OS and Linux.
  // TODO(alex): By now we have no better way to extract the kill
  // reason. Change this once we have level 2 enums for task states.
  EXPECT_TRUE(statusKilled.get().has_message());
  EXPECT_TRUE(strings::contains(statusKilled.get().message(), "Terminated"))
    << statusKilled.get().message();

  // Stop the driver while the task is running.
  driver.stop();
  driver.join();

  Shutdown();  // Must shutdown before 'containerizer' gets deallocated.
  delete containerizer.get();
}
