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
#include <memory>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>
#include <process/subprocess.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"
#include "common/http.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/limiter.hpp"
#include "tests/mesos.hpp"

using namespace mesos::internal::slave;

using mesos::internal::master::Master;

using process::Clock;
using process::Future;
using process::PID;
using process::Promise;
using process::UPID;

using std::map;
using std::shared_ptr;
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
using testing::Sequence;

namespace mesos {
namespace internal {
namespace tests {

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more slave than master-related are in this file.
// The others are in "master_tests.cpp".

class SlaveTest : public MesosTest {};


// This test ensures that when a slave shuts itself down, it
// unregisters itself and the master notifies the framework
// immediately and rescinds any offers.
TEST_F(SlaveTest, Shutdown)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers.get().size());

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, offers.get()[0].id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, offers.get()[0].slave_id()))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Stop the checkpointing slave with explicit shutdown message
  // so that the slave unregisters.
  Stop(slave.get(), true);

  AWAIT_READY(offerRescinded);
  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unregistered"]);

  driver.stop();
  driver.join();
}


TEST_F(SlaveTest, ShutdownUnregisteredExecutor)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  // Set the isolation flag so we know a MesoContainerizer will be created.
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
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
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status.get().source());

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that when an executor terminates before
// registering with slave, it is properly cleaned up.
TEST_F(SlaveTest, RemoveUnregisteredTerminatedExecutor)
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
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status.get().source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, status.get().reason());

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
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;

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
  map<string, string> environment = os::environment();
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
  Future<TaskStatus> statusRunning, statusFinished;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  Try<Subprocess> executor =
    subprocess(
        executorCommand,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        environment);

  ASSERT_SOME(executor);

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning.get().source());

  AWAIT_READY(statusFinished);
  ASSERT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished.get().source());

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
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
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

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning.get().source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished.get().source());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Don't let args from the CommandInfo struct bleed over into
// mesos-executor forking. For more details of this see MESOS-1873.
TEST_F(SlaveTest, GetExecutorInfo)
{
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);

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
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
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

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning.get().source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished.get().source());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test runs a command _with_ the command user field set. The
// command will verify the assumption that the command is run as the
// specified user. We use (and assume the presence) of the
// unprivileged 'nobody' user which should be available on both Linux
// and Mac OS X.
TEST_F(SlaveTest, DISABLED_ROOT_RunTaskWithCommandInfoWithUser)
{
  // TODO(nnielsen): Introduce STOUT abstraction for user verification
  // instead of flat getpwnam call.
  const string testUser = "nobody";
  if (::getpwnam(testUser.c_str()) == NULL) {
    LOG(WARNING) << "Cannot run ROOT_RunTaskWithCommandInfoWithUser test:"
                 << " user '" << testUser << "' is not present";
    return;
  }

  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  const string helper =
      path::join(tests::flags.build_dir, "src", "active-user-test-helper");

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // HACK: Launch a prepare task as root to prepare the binaries.
  // This task creates the lt-mesos-executor binary in the build dir.
  // Because the real task is run as a test user (nobody), it does not
  // have permission to create files in the build directory.
  TaskInfo prepareTask;
  prepareTask.set_name("prepare task");
  prepareTask.mutable_task_id()->set_value("1");
  prepareTask.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  prepareTask.mutable_resources()->CopyFrom(
      offers.get()[0].resources());

  Result<string> user = os::user();
  CHECK_SOME(user) << "Failed to get current user name"
                   << (user.isError() ? ": " + user.error() : "");
  // Current user should be root.
  EXPECT_EQ("root", user.get());

  // This prepare command executor will run as the current user
  // running the tests (root). After this command executor finishes,
  // we know that the lt-mesos-executor binary file exists.
  CommandInfo prepareCommand;
  prepareCommand.set_shell(false);
  prepareCommand.set_value(helper);
  prepareCommand.add_arguments(helper);
  prepareCommand.add_arguments(user.get());
  prepareTask.mutable_command()->CopyFrom(prepareCommand);

  vector<TaskInfo> prepareTasks;
  prepareTasks.push_back(prepareTask);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), prepareTasks);

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning.get().source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished.get().source());

  // Start to launch a task with different user.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("2");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_user(testUser);
  command.set_shell(false);
  command.set_value(helper);
  command.add_arguments(helper);
  command.add_arguments(testUser);

  task.mutable_command()->CopyFrom(command);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning.get().source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished.get().source());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that a status update acknowledgement from a
// non-leading master is ignored.
TEST_F(SlaveTest, IgnoreNonLeaderStatusUpdateAcknowledgement)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
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


TEST_F(SlaveTest, MetricsInMetricsEndpoint)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  JSON::Object snapshot = Metrics();

  EXPECT_EQ(1u, snapshot.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, snapshot.values.count("slave/registered"));

  EXPECT_EQ(1u, snapshot.values.count("slave/recovery_errors"));

  EXPECT_EQ(1u, snapshot.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_lost"));

  EXPECT_EQ(1u, snapshot.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminated"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(
      1u,
      snapshot.values.count("slave/executor_directory_max_allowed_age_secs"));

  EXPECT_EQ(1u, snapshot.values.count("slave/container_launch_errors"));

  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/mem_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/disk_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_percent"));

  Shutdown();
}


// Test to verify that we increment the container launch errors metric
// when we fail to launch a container.
TEST_F(SlaveTest, MetricsSlaveLaunchErrors)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  const Offer offer = offers.get()[0];

  // Verify that we start with no launch failures.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(0, snapshot.values["slave/container_launch_errors"]);

  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(Return(Failure("Injected failure")));

  EXPECT_CALL(sched, statusUpdate(&driver, _));

  // Try to start a task
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      "sleep 1000",
      DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offer.id(), {task});

  // After failure injection, metrics should report a single failure.
  snapshot = Metrics();
  EXPECT_EQ(1, snapshot.values["slave/container_launch_errors"]);

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(SlaveTest, StateEndpoint)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  flags.hostname = "localhost";
  flags.resources = "cpus:4;mem:2048;disk:512;ports:[33000-34000]";
  flags.attributes = "rack:abc;host:myhost";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  // Capture the start time deterministically.
  Clock::pause();

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  Future<process::http::Response> response =
    process::http::get(slave.get(), "state.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  EXPECT_EQ(MESOS_VERSION, state.values["version"]);

  if (build::GIT_SHA.isSome()) {
    EXPECT_EQ(build::GIT_SHA.get(), state.values["git_sha"]);
  }

  if (build::GIT_BRANCH.isSome()) {
    EXPECT_EQ(build::GIT_BRANCH.get(), state.values["git_branch"]);
  }

  if (build::GIT_TAG.isSome()) {
    EXPECT_EQ(build::GIT_TAG.get(), state.values["git_tag"]);
  }

  EXPECT_EQ(build::DATE, state.values["build_date"]);
  EXPECT_EQ(build::TIME, state.values["build_time"]);
  EXPECT_EQ(build::USER, state.values["build_user"]);

  ASSERT_TRUE(state.values["start_time"].is<JSON::Number>());
  EXPECT_EQ(
      static_cast<int>(Clock::now().secs()),
      static_cast<int>(state.values["start_time"].as<JSON::Number>().value));

  // TODO(bmahler): The slave must register for the 'id'
  // to be non-empty.
  ASSERT_TRUE(state.values["id"].is<JSON::String>());

  EXPECT_EQ(stringify(slave.get()), state.values["pid"]);
  EXPECT_EQ(flags.hostname.get(), state.values["hostname"]);

  Try<Resources> resources = Resources::parse(
      flags.resources.get(), flags.default_role);

  ASSERT_SOME(resources);

  EXPECT_EQ(model(resources.get()), state.values["resources"]);

  Attributes attributes = Attributes::parse(flags.attributes.get());

  EXPECT_EQ(model(attributes), state.values["attributes"]);

  // TODO(bmahler): Test "master_hostname", "log_dir",
  // "external_log_file".

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["completed_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["completed_frameworks"].as<JSON::Array>().values.empty());

  // TODO(bmahler): Ensure this contains all the flags.
  ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
  EXPECT_FALSE(state.values["flags"].as<JSON::Object>().values.empty());

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

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  const vector<TaskInfo> tasks = {task};

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  response = http::get(slave.get(), "state.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Check that executor_id is in the right format.
  ASSERT_SOME_EQ(
      "default",
      parse.get().find<JSON::String>(
          "frameworks[0].executors[0].tasks[0].executor_id"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that when a slave is shutting down, it will not
// try to re-register with the master.
TEST_F(SlaveTest, TerminatingSlaveDoesNotReregister)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
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
  Try<PID<Slave>> slave = StartSlave(&exec, &detector, flags);
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
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  EXPECT_CALL(exec, registered(_, _, _, _));

  TestContainerizer containerizer(&exec);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

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
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, status3.get().source());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_LOST, status4.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status4.get().source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, status4.get().reason());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the resources of a container will be
// updated before tasks are sent to the executor.
TEST_F(SlaveTest, ContainerUpdatedBeforeTaskReachesExecutor)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  EXPECT_CALL(exec, registered(_, _, _, _));

  TestContainerizer containerizer(&exec);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, "1", "128", "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // This is used to determine which of the following finishes first:
  // `containerizer->update` or `exec->launchTask`. We want to make
  // sure that containerizer update always finishes before the task is
  // sent to the executor.
  Sequence sequence;

  EXPECT_CALL(containerizer, update(_, _))
    .InSequence(sequence)
    .WillOnce(Return(Nothing()));

  EXPECT_CALL(exec, launchTask(_, _))
    .InSequence(sequence)
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies the slave will destroy a container if updating
// the container's resources fails during task launch.
TEST_F(SlaveTest, TaskLaunchContainerizerUpdateFails)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, "1", "128", "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // The executor may not receive the ExecutorRegisteredMessage if the
  // container is destroyed before that.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  // Set up the containerizer so update() will fail.
  EXPECT_CALL(containerizer, update(_, _))
    .WillOnce(Return(Failure("update() failed")))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status.get().source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, status.get().reason());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that the slave will re-register with the master
// if it does not receive any pings after registering.
TEST_F(SlaveTest, PingTimeoutNoPings)
{
  // Set shorter ping timeout values.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.slave_ping_timeout = Seconds(5);
  masterFlags.max_slave_ping_timeouts = 2u;
  Duration totalTimeout =
    masterFlags.slave_ping_timeout * masterFlags.max_slave_ping_timeouts;

  // Start a master.
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Block all pings to the slave.
  DROP_MESSAGES(Eq("PING"), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_TRUE(slaveRegisteredMessage.get().has_connection());
  MasterSlaveConnection connection = slaveRegisteredMessage.get().connection();
  EXPECT_EQ(totalTimeout, Seconds(connection.total_ping_timeout_seconds()));

  // Ensure the slave processes the registration message and schedules
  // the ping timeout, before we advance the clock.
  Clock::pause();
  Clock::settle();

  // Advance to the ping timeout to trigger a re-detection and
  // re-registration.
  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(totalTimeout);

  AWAIT_READY(detected);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that the slave will re-register with the master
// if it stops receiving pings.
TEST_F(SlaveTest, PingTimeoutSomePings)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  Clock::pause();

  // Ensure a ping reaches the slave.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  Clock::advance(masterFlags.slave_ping_timeout);

  AWAIT_READY(ping);

  // Now block further pings from the master and advance
  // the clock to trigger a re-detection and re-registration on
  // the slave.
  DROP_MESSAGES(Eq("PING"), _, _);

  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(slave::DEFAULT_MASTER_PING_TIMEOUT());

  AWAIT_READY(detected);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that when slave removal rate limit is specified
// a slave that fails health checks is removed after a permit is
// provided by the rate limiter.
TEST_F(SlaveTest, RateLimitSlaveShutdown)
{
  // Start a master.
  shared_ptr<MockRateLimiter> slaveRemovalLimiter(new MockRateLimiter());
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_MESSAGES(Eq("PONG"), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  Future<ShutdownMessage> shutdown = FUTURE_PROTOBUF(ShutdownMessage(), _, _);

  // Induce a health check failure of the slave.
  Clock::pause();
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_slave_ping_timeouts) {
      Clock::advance(masterFlags.slave_ping_timeout);
      break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(masterFlags.slave_ping_timeout);
  }

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The shutdown should not occur before the permit is satisfied.
  Clock::settle();
  ASSERT_TRUE(shutdown.isPending());

  // Once the permit is satisfied, the shutdown message
  // should be sent.
  promise.set(Nothing());
  AWAIT_READY(shutdown);
}


// This test verifies that when a slave responds to pings after the
// slave observer has scheduled it for shutdown (due to health check
// failure), the shutdown is cancelled.
TEST_F(SlaveTest, CancelSlaveShutdown)
{
  // Start a master.
  shared_ptr<MockRateLimiter> slaveRemovalLimiter(new MockRateLimiter());
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_MESSAGES(Eq("PONG"), _, _);

  // No shutdown should occur during the test!
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Induce a health check failure of the slave.
  Clock::pause();
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_slave_ping_timeouts) {
      Clock::advance(masterFlags.slave_ping_timeout);
      break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(masterFlags.slave_ping_timeout);
  }

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // Settle to make sure the shutdown does not occur.
  Clock::settle();

  // Reset the filters to allow pongs from the slave.
  filter(NULL);

  // Advance clock enough to do a ping pong.
  Clock::advance(masterFlags.slave_ping_timeout);
  Clock::settle();

  // The master should have tried to cancel the removal.
  ASSERT_TRUE(promise.future().hasDiscard());

  // Allow the cancelation and settle the clock to ensure a shutdown
  // does not occur.
  promise.discard();
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
  Try<PID<Master>> master = StartMaster();
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

  // Skip what Slave::_runTask() normally does, save its arguments for
  // later, tie reaching the critical moment when to kill the task to
  // a future.
  Future<Nothing> _runTask;
  EXPECT_CALL(slave, _runTask(_, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_runTask),
                    SaveArg<0>(&future),
                    SaveArg<1>(&frameworkInfo)));

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
      future, frameworkInfo, master.get(), task);

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
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Create a StandaloneMasterDetector to enable the slave to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get());

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&exec, &detector);
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


// This test verifies that the slave should properly handle the case
// where the containerizer usage call fails when getting the usage
// information.
TEST_F(SlaveTest, ContainerizerUsageFailure)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get());

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      "sleep 1000",
      exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Set up the containerizer so the next usage() will fail.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(Failure("Injected failure")));

  // We expect that the slave will still returns ResourceUsage but no
  // statistics will be found.
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);
  ASSERT_EQ(1, usage.get().executors_size());
  EXPECT_FALSE(usage.get().executors(0).has_statistics());

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);

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
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in slave state.json.
  Future<process::http::Response> response =
    process::http::get(slave.get(), "state.json");
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


// Test that we can set the executors environment variables and it
// won't inhert the slaves.
TEST_F(SlaveTest, ExecutorEnvironmentVariables)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_environment_variables'.
  slave::Flags flags = CreateSlaveFlags();

  Try<JSON::Object> parse = JSON::parse<JSON::Object>("{\"PATH\": \"/bin\"}");

  ASSERT_SOME(parse);

  flags.executor_environment_variables = parse.get();

  Try<PID<Slave>> slave = StartSlave(flags);
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

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(true);
  command.set_value("test $PATH = /bin");

  task.mutable_command()->MergeFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the slave should properly show total slave
// resources.
TEST_F(SlaveTest, TotalSlaveResourcesIncludedInUsage)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;
  StandaloneMasterDetector detector(master.get());

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem:1024;disk:1024;ports:[31000-32000]";

  MockSlave slave(flags, &detector, &containerizer);
  spawn(slave);

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  // We expect that the slave will return ResourceUsage with
  // total resources reported.
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);

  // Total resources should match the resources from flag.resources.
  EXPECT_EQ(Resources(usage.get().total()),
            Resources::parse(flags.resources.get()).get());

  terminate(slave);
  wait(slave);

  Shutdown();
}


// This test verifies that the slave should properly show total slave
// resources with checkpointed resources applied.
TEST_F(SlaveTest, CheckpointedResourcesIncludedInUsage)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;
  StandaloneMasterDetector detector(master.get());

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;cpus(role1):3;mem:1024;disk:1024;disk(role1):64;"
                    "ports:[31000-32000]";

  MockSlave slave(flags, &detector, &containerizer);
  spawn(slave);

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  Resource dynamicReservation = Resources::parse("cpus", "1", "role1").get();
  dynamicReservation.mutable_reservation()->CopyFrom(
      createReservationInfo("principal"));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  vector<Resource> checkpointedResources =
    {dynamicReservation, persistentVolume};

  // Add checkpointed resources.
  slave.checkpointResources(checkpointedResources);

  // We expect that the slave will return ResourceUsage with
  // total and checkpointed slave resources reported.
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);

  Resources usageTotalResources(usage.get().total());

  // Reported total field should contain persistent volumes and dynamic
  // reservations.
  EXPECT_EQ(usageTotalResources.persistentVolumes(), persistentVolume);
  EXPECT_TRUE(usageTotalResources.contains(dynamicReservation));

  terminate(slave);
  wait(slave);

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
