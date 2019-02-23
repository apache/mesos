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

#include <list>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/pstree.hpp>
#include <stout/os/read.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/su.hpp>

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/launcher.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::SubprocessLauncher;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::Slave;

using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::RunState;
using mesos::internal::slave::state::SlaveState;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerLogger;
using mesos::slave::Isolator;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
const char LOGROTATE_CONTAINER_LOGGER_NAME[] =
  "org_apache_mesos_LogrotateContainerLogger";
#endif // __WINDOWS__


class ContainerLoggerTest : public MesosTest {};


// Tests that the default container logger writes files into the sandbox.
TEST_F(ContainerLoggerTest, DefaultToSandbox)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // We'll start a task that outputs to stdout.
  TaskInfo task = createTask(offers.get()[0], "echo 'Hello World!'");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  slave->reset();

  // Check that the sandbox was written to.
  string sandboxDirectory = path::join(
      slave::paths::getExecutorPath(
          flags.work_dir,
          slaveId,
          frameworkId.get(),
          statusRunning->executor_id()),
      "runs",
      "latest");

  ASSERT_TRUE(os::exists(sandboxDirectory));

  string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  Result<string> stdout = os::read(stdoutPath);
  ASSERT_SOME(stdout);
  EXPECT_TRUE(strings::contains(stdout.get(), "Hello World!"));
}


// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
// Tests that the packaged logrotate container logger writes files into the
// sandbox and keeps them at a reasonable size.
TEST_F(ContainerLoggerTest, LOGROTATE_RotateInSandbox)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  // Use the non-default container logger that rotates logs.
  flags.container_logger = LOGROTATE_CONTAINER_LOGGER_NAME;

  Fetcher fetcher(flags);

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a task that spams stdout with 11 MB of (mostly blank) output.
  // The logrotate container logger module is loaded with parameters that limit
  // the log size to five files of 2 MB each.  After the task completes, there
  // should be five files with a total size of 9 MB.  The first 2 MB file
  // should have been deleted.  The "stdout" file should be 1 MB large.
  TaskInfo task = createTask(
      offers.get()[0],
      "i=0; while [ $i -lt 11264 ]; "
      "do printf '%-1024d\\n' $i; i=$((i+1)); done");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  // The `LogrotateContainerLogger` spawns some `mesos-logrotate-logger`
  // processes above, which continue running briefly after the container exits.
  // Once they finish reading the container's pipe, they should exit.
  Try<os::ProcessTree> pstrees = os::pstree(0);
  ASSERT_SOME(pstrees);
  foreach (const os::ProcessTree& pstree, pstrees->children) {
    // Wait for the logger subprocesses to exit, for up to 5 seconds each.
    Duration waited = Duration::zero();
    do {
      if (!os::exists(pstree.process.pid)) {
        break;
      }

      // Push the clock ahead to speed up the reaping of subprocesses.
      Clock::pause();
      Clock::settle();
      Clock::advance(Seconds(1));
      Clock::resume();

      os::sleep(Milliseconds(100));
      waited += Milliseconds(100);
    } while (waited < Seconds(5));

    EXPECT_LE(waited, Seconds(5));
  }

  // Check for the expected log rotation.
  string sandboxDirectory = path::join(
      slave::paths::getExecutorPath(
          flags.work_dir,
          slaveId,
          frameworkId.get(),
          statusRunning->executor_id()),
      "runs",
      "latest");

  ASSERT_TRUE(os::exists(sandboxDirectory));

  // The leading log file should be about half full (1 MB).
  string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  // NOTE: We don't expect the size of the leading log file to be precisely
  // one MB since there is also the executor's output besides the task's stdout.
  Try<Bytes> stdoutSize = os::stat::size(stdoutPath);
  ASSERT_SOME(stdoutSize);
  EXPECT_LE(1024u, stdoutSize->bytes() / Bytes::KILOBYTES);
  EXPECT_GE(1050u, stdoutSize->bytes() / Bytes::KILOBYTES);

  // We should only have files up to "stdout.4".
  stdoutPath = path::join(sandboxDirectory, "stdout.5");
  EXPECT_FALSE(os::exists(stdoutPath));

  // The next four rotated log files (2 MB each) should be present.
  for (int i = 1; i < 5; i++) {
    stdoutPath = path::join(sandboxDirectory, "stdout." + stringify(i));
    ASSERT_TRUE(os::exists(stdoutPath));

    // NOTE: The rotated files are written in contiguous blocks, meaning that
    // each file may be less than the maximum allowed size.
    stdoutSize = os::stat::size(stdoutPath);
    EXPECT_LE(2040u, stdoutSize->bytes() / Bytes::KILOBYTES);
    EXPECT_GE(2048u, stdoutSize->bytes() / Bytes::KILOBYTES);
  }
}


// Tests that the packaged logrotate container logger will find and use
// overrides inside the Executor's environment.
TEST_F(ContainerLoggerTest, LOGROTATE_CustomRotateOptions)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  // Use the non-default container logger that rotates logs.
  flags.container_logger = LOGROTATE_CONTAINER_LOGGER_NAME;

  Fetcher fetcher(flags);

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const string testFile = sandbox.get() + "/CustomRotateOptions";

  // Custom config consists of a postrotate script which creates
  // an empty file in the temporary directory on log rotation.
  const string customConfig =
    "postrotate\n touch " + testFile + "\nendscript";

  // Start a task that spams stdout with 2 MB of (mostly blank) output.
  // The logrotate container logger module is loaded with parameters that limit
  // the log size to five files of 2 MB each.  After the task completes,
  // `logrotate` should trigger rotation of stdout logs, so postrotate
  // script is executed.
  TaskInfo task = createTask(
      offers.get()[0],
      "i=0; while [ $i -lt 2048 ]; "
      "do printf '%-1024d\\n' $i; i=$((i+1)); done");

  // Add an override for the logger's stdout stream.
  // We will check this by inspecting the generated configuration file.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_LOGROTATE_STDOUT_OPTIONS");
  variable->set_value(customConfig);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  // The `LogrotateContainerLogger` spawns some `mesos-logrotate-logger`
  // processes above, which continue running briefly after the container exits.
  // Once they finish reading the container's pipe, they should exit.
  Try<os::ProcessTree> pstrees = os::pstree(0);
  ASSERT_SOME(pstrees);
  foreach (const os::ProcessTree& pstree, pstrees->children) {
    // Wait for the logger subprocesses to exit, for up to 5 seconds each.
    Duration waited = Duration::zero();
    do {
      if (!os::exists(pstree.process.pid)) {
        break;
      }

      // Push the clock ahead to speed up the reaping of subprocesses.
      Clock::pause();
      Clock::settle();
      Clock::advance(Seconds(1));
      Clock::resume();

      os::sleep(Milliseconds(100));
      waited += Milliseconds(100);
    } while (waited < Seconds(5));

    EXPECT_LE(waited, Seconds(5));
  }

  // Check for the expected logger files.
  string sandboxDirectory = path::join(
      slave::paths::getExecutorPath(
          flags.work_dir,
          slaveId,
          frameworkId.get(),
          statusRunning->executor_id()),
      "runs",
      "latest");

  ASSERT_TRUE(os::exists(sandboxDirectory));

  string stdoutPath = path::join(sandboxDirectory, "stdout.logrotate.conf");

  // Check that `mesos-logrotate-logger` creates config file in the container
  // sandbox when `ENABLE_LAUNCHER_SEALING` flag is not specified. Otherwise,
  // the config file is created in `procfs`, so it should not exist in the
  // container sandbox.
#ifdef ENABLE_LAUNCHER_SEALING
  ASSERT_FALSE(os::exists(stdoutPath));
#else
  ASSERT_TRUE(os::exists(stdoutPath));
#endif // ENABLE_LAUNCHER_SEALING

  // This file should be created by a script on log rotation. The script is
  // specified via logrotate stdout options.
  ASSERT_TRUE(os::exists(testFile));
}


// Tests that the logrotate container logger only closes FDs when it
// is supposed to and does not interfere with other FDs on the agent.
TEST_F(ContainerLoggerTest, LOGROTATE_ModuleFDOwnership)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  // Use the non-default container logger that rotates logs.
  flags.container_logger = LOGROTATE_CONTAINER_LOGGER_NAME;

  Fetcher fetcher(flags);

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a task that will keep running until the end of the test.
  TaskInfo task = createTask(offers.get()[0], "sleep 100");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusKilled))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Open multiple files, so that we're fairly certain we've opened
  // the same FDs (integers) opened by the container logger.
  vector<int> fds;
  for (int i = 0; i < 50; i++) {
    Try<int> fd = os::open(os::DEV_NULL, O_RDONLY);
    ASSERT_SOME(fd);

    fds.push_back(fd.get());
  }

  // Kill the task, which also kills the executor.
  driver.killTask(statusRunning->task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  AWAIT_READY(executorTerminated);

  // Close all the FDs we opened.  Every `close` should succeed.
  foreach (int fd, fds) {
    ASSERT_SOME(os::close(fd));
  }

  driver.stop();
  driver.join();
}


// These tests are parameterized by the boolean `--switch-user` agent flag.
class UserContainerLoggerTest
  : public ContainerLoggerTest, public WithParamInterface<bool> {};

INSTANTIATE_TEST_CASE_P(
    bool,
    UserContainerLoggerTest,
    ::testing::Values(true, false));


// Tests that the packaged logrotate container logger will rotate files when
// the agent is root, but the executor is launched as a non-root user.
//
// 1. When `--switch_user` is true on the agent, the logger module should
//    launch subprocesses with the same user as the executor.
// 2. When `--switch_user` is false on the agent, the logger module should
//    inherit the user of the agent.
TEST_P(UserContainerLoggerTest,
       ROOT_LOGROTATE_UNPRIVILEGED_USER_RotateWithSwitchUserTrueOrFalse)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  // Use the non-default container logger that rotates logs.
  flags.container_logger = LOGROTATE_CONTAINER_LOGGER_NAME;

  // Parameterize the `--switch_user` flag to test both options.
  flags.switch_user = GetParam();

  // In order for the unprivileged user to successfully chdir, the
  // agent's work directory needs to have execute permissions.
  Try<Nothing> chmod = os::chmod(
      flags.work_dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  ASSERT_SOME(chmod);

  Fetcher fetcher(flags);

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a task that spams stdout with 3 MB of (mostly blank) output.
  // The logrotate container logger module is loaded with parameters that limit
  // the log size to five files of 2 MB each.  After the task completes, there
  // should be two files with a total size of 3 MB.  The "stdout" file should
  // be 1 MB large.
  TaskInfo task = createTask(
      offers.get()[0],
      "i=0; while [ $i -lt 3072 ]; "
      "do printf '%-1024d\\n' $i; i=$((i+1)); done");

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  // Start the task as a non-root user.
  task.mutable_command()->set_user(user.get());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  // The `LogrotateContainerLogger` spawns some `mesos-logrotate-logger`
  // processes above, which continue running briefly after the container exits.
  // Once they finish reading the container's pipe, they should exit.
  Try<os::ProcessTree> pstrees = os::pstree(0);
  ASSERT_SOME(pstrees);
  foreach (const os::ProcessTree& pstree, pstrees->children) {
    // Wait for the logger subprocesses to exit, for up to 5 seconds each.
    Duration waited = Duration::zero();
    do {
      if (!os::exists(pstree.process.pid)) {
        break;
      }

      // Push the clock ahead to speed up the reaping of subprocesses.
      Clock::pause();
      Clock::settle();
      Clock::advance(Seconds(1));
      Clock::resume();

      os::sleep(Milliseconds(100));
      waited += Milliseconds(100);
    } while (waited < Seconds(5));

    EXPECT_LE(waited, Seconds(5));
  }

  // Check for the expected log rotation.
  string sandboxDirectory = path::join(
      slave::paths::getExecutorPath(
          flags.work_dir,
          slaveId,
          frameworkId.get(),
          statusRunning->executor_id()),
      "runs",
      "latest");

  ASSERT_TRUE(os::exists(sandboxDirectory));

  // The leading log file should be owned by the logrotate module's
  // companion binary's user.
  string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  struct stat stdoutStat;
  ASSERT_GE(::stat(stdoutPath.c_str(), &stdoutStat), 0);

  // Depending on the `--switch_user`, the expected user is either
  // "$SUDO_USER" or "root".
  Result<string> stdoutUser = os::user(stdoutStat.st_uid);
  if (GetParam()) {
    ASSERT_SOME_EQ(user.get(), stdoutUser);
  } else {
    ASSERT_SOME_EQ("root", stdoutUser);
  }

  // The leading log file should be about half full (1 MB).
  // NOTE: We don't expect the size of the leading log file to be precisely
  // one MB since there is also the executor's output besides the task's stdout.
  Try<Bytes> stdoutSize = os::stat::size(stdoutPath);
  ASSERT_SOME(stdoutSize);
  EXPECT_LE(1024u, stdoutSize->bytes() / Bytes::KILOBYTES);
  EXPECT_GE(1050u, stdoutSize->bytes() / Bytes::KILOBYTES);

  // We should only have files up to "stdout.1".
  stdoutPath = path::join(sandboxDirectory, "stdout.2");
  EXPECT_FALSE(os::exists(stdoutPath));

  // The only rotated log file (2 MB each) should be present.
  stdoutPath = path::join(sandboxDirectory, "stdout.1");
  ASSERT_TRUE(os::exists(stdoutPath));

  // NOTE: The rotated files are written in contiguous blocks, meaning that
  // each file may be less than the maximum allowed size.
  stdoutSize = os::stat::size(stdoutPath);
  ASSERT_SOME(stdoutSize);
  EXPECT_LE(2040u, stdoutSize->bytes() / Bytes::KILOBYTES);
  EXPECT_GE(2048u, stdoutSize->bytes() / Bytes::KILOBYTES);
}
#endif // __WINDOWS__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
