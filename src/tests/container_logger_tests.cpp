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

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
