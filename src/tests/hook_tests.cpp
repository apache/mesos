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

#include <mesos/module.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "hook/manager.hpp"

#include "module/manager.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::string;

using namespace mesos::modules;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::string;
using std::vector;

using testing::_;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

const char* HOOK_MODULE_LIBRARY_NAME = "testhook";
const char* HOOK_MODULE_NAME = "org_apache_mesos_TestHook";

// Must be kept in sync with variables of the same name in
// examples/test_hook_module.cpp.
const char* testLabelKey = "MESOS_Test_Label";
const char* testLabelValue = "ApacheMesos";
const char* testEnvironmentVariableName = "MESOS_TEST_ENVIRONMENT_VARIABLE";

class HookTest : public MesosTest
{
protected:
  // TODO(karya): Replace constructor/destructor with SetUp/TearDown.
  // Currently, using SetUp/TearDown causes VerifySlave* test to
  // fail with a duplicate slave id message.  However, everything
  // seems normal when using this construction/destructor combo.
  HookTest()
  {
    // Install hooks.
    EXPECT_SOME(HookManager::initialize(HOOK_MODULE_NAME));
  }

  ~HookTest()
  {
    // Unload the hooks so a subsequent install may succeed.
    EXPECT_SOME(HookManager::unload(HOOK_MODULE_NAME));
  }
};


// Test varioud hook install/uninstall mechanisms.
TEST_F(HookTest, HookLoading)
{
  // Installing unknown hooks should fail.
  EXPECT_ERROR(HookManager::initialize("Unknown Hook"));

  // Uninstalling an unknown hook should fail.
  EXPECT_ERROR(HookManager::unload("Unknown Hook"));

  // Installing an already installed hook should fail.
  EXPECT_ERROR(HookManager::initialize(HOOK_MODULE_NAME));

  // Uninstalling a hook should succeed.
  EXPECT_SOME(HookManager::unload(HOOK_MODULE_NAME));

  // Uninstalling an already uninstalled hook should fail.
  EXPECT_ERROR(HookManager::unload(HOOK_MODULE_NAME));
  // This is needed to allow the tear-down to succeed.
  EXPECT_SOME(HookManager::initialize(HOOK_MODULE_NAME));
}


// Test that the label decorator hook hangs a new label off the
// taskinfo message during master launch task.
TEST_F(HookTest, VerifyMasterLaunchTaskHook)
{
  Try<PID<Master>> master = StartMaster(CreateMasterFlags());
  ASSERT_SOME(master);

  TestContainerizer containerizer;

  StandaloneMasterDetector detector(master.get());

  // Start a mock slave since we aren't testing the slave hooks yet.
  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  process::spawn(slave);

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

  CommandInfo command;
  command.set_value("sleep 10");

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_command()->CopyFrom(command);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskInfo> taskInfo;
  EXPECT_CALL(slave, runTask(_, _, _, _, _))
    .Times(1)
    .WillOnce(FutureArg<4>(&taskInfo));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(taskInfo);

  // At launchTasks, the label decorator hook inside should have been
  // executed and we should see the labels now.
  Option<string> labelValue;
  foreach (const Label& label, taskInfo.get().labels().labels()) {
    if (label.key() == testLabelKey) {
      labelValue = label.value();
    }
  }
  EXPECT_SOME_EQ(testLabelValue, labelValue);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test that the environment decorator hook adds a new environment
// variable to the executor runtime.
// Test hook adds a new environment variable "FOO" to the executor
// with a value "bar". We validate the hook by verifying the value
// of this environment variable.
TEST_F(HookTest, VerifySlaveExecutorEnvironmentDecorator)
{
  const string& directory = os::getcwd(); // We're inside a temporary sandbox.
  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(CreateSlaveFlags(), false, &fetcher);
  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  // Test hook adds a new environment variable "FOO" to the executor
  // with a value "bar". A '0' (success) exit status for the following
  // command validates the hook.
  process::Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "test $FOO = 'bar'"),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
      false);
  AWAIT_READY(launch);
  ASSERT_TRUE(launch.get());

  // Wait on the container.
  process::Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);
  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  delete containerizer.get();
}


// Test executor environment decorator hook and remove executor hook
// for slave.  We expect the environment-decorator hook to create a
// temporary file and the remove-executor hook to delete that file.
TEST_F(HookTest, DISABLED_VerifySlaveLaunchExecutorHook)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
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
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, launchTask(_, _));

  Future<ExecutorInfo> executorInfo;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<1>(&executorInfo));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(executorInfo);

  // At launchTasks, the label decorator hook inside should have been
  // executed and we should see the labels now.
  // Further, the environment decorator hook should also have been
  // executed.  In that hook, we create a temp file and set the path
  // as the value of the environment variable.
  // Here we verify that the environment variable is present and the
  // file is present on the disk.
  Option<string> path;
  foreach (const Environment::Variable& variable,
           executorInfo.get().command().environment().variables()) {
    if (variable.name() == testEnvironmentVariableName) {
      path = variable.value();
      break;
    }
  }

  EXPECT_SOME(path);
  // The file must have been create by the environment decorator hook.
  EXPECT_TRUE(os::stat::isfile(path.get()));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.

  // The removeExecutor hook in the test module deletes the temp file.
  // Verify that the file is not present.
  EXPECT_FALSE(os::stat::isfile(path.get()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
