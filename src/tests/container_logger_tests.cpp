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

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/pstree.hpp>
#include <stout/os/stat.hpp>

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

const char LOGROTATE_CONTAINER_LOGGER_NAME[] =
  "org_apache_mesos_LogrotateContainerLogger";


class ContainerLoggerTest : public MesosTest {};


// Tests that the default container logger writes files into the sandbox.
TEST_F(ContainerLoggerTest, DefaultToSandbox)
{
  // Create a master, agent, and framework.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

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
  EXPECT_NE(0u, offers.get().size());

  // We'll start a task that outputs to stdout.
  TaskInfo task = createTask(offers.get()[0], "echo 'Hello World!'");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Check that the sandbox was written to.
  string sandboxDirectory = path::join(
      slave::paths::getExecutorPath(
          flags.work_dir,
          slaveId,
          frameworkId.get(),
          status->executor_id()),
      "runs",
      "latest");

  ASSERT_TRUE(os::exists(sandboxDirectory));

  string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  Result<string> stdout = os::read(stdoutPath);
  ASSERT_SOME(stdout);
  EXPECT_TRUE(strings::contains(stdout.get(), "Hello World!"));

  driver.stop();
  driver.join();

  Shutdown();
}


// Tests that the packaged logrotate container logger writes files into the
// sandbox and keeps them at a reasonable size.
TEST_F(ContainerLoggerTest, LOGROTATE_RotateInSandbox)
{
  // Create a master, agent, and framework.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // We'll need access to these flags later.
  slave::Flags flags = CreateSlaveFlags();

  // Use the non-default container logger that rotates logs.
  flags.container_logger = LOGROTATE_CONTAINER_LOGGER_NAME;

  Fetcher fetcher;

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

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
  EXPECT_NE(0u, offers.get().size());

  // Start a task that spams stdout with 11 MB of (mostly blank) output.
  // The logrotate container logger module is loaded with parameters that limit
  // the log size to five files of 2 MB each.  After the task completes, there
  // should be five files with a total size of 9 MB.  The first 2 MB file
  // should have been deleted.  The "stdout" file should be 1 MB large.
  TaskInfo task = createTask(
      offers.get()[0],
      "i=0; while [ $i -lt 11264 ]; "
      "do printf '%-1024d\\n' $i; i=$((i+1)); done");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown();

  // The `LogrotateContainerLogger` spawns some `mesos-logrotate-logger`
  // processes above, which continue running briefly after the container exits.
  // Once they finish reading the container's pipe, they should exit.
  Try<os::ProcessTree> pstrees = os::pstree(0);
  ASSERT_SOME(pstrees);
  foreach (const os::ProcessTree& pstree, pstrees.get().children) {
    ASSERT_EQ(pstree.process.pid, waitpid(pstree.process.pid, NULL, 0));
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
  EXPECT_LE(1024, stdoutSize->kilobytes());
  EXPECT_GE(1050, stdoutSize->kilobytes());

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
    EXPECT_LE(2040, stdoutSize->kilobytes());
    EXPECT_GE(2048, stdoutSize->kilobytes());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
