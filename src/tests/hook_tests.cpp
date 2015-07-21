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

#include "master/flags.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#include "module/manager.hpp"

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
using testing::DoAll;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

const char* HOOK_MODULE_LIBRARY_NAME = "testhook";
const char* HOOK_MODULE_NAME = "org_apache_mesos_TestHook";

// Must be kept in sync with variables of the same name in
// examples/test_hook_module.cpp.
const char* testLabelKey = "MESOS_Test_Label";
const char* testLabelValue = "ApacheMesos";
const char* testRemoveLabelKey = "MESOS_Test_Remove_Label";
const char* testRemoveLabelValue = "FooBar";
const char* testEnvironmentVariableName = "MESOS_TEST_ENVIRONMENT_VARIABLE";

class HookTest : public MesosTest
{
protected:
  // TODO(karya): Replace constructor/destructor with SetUp/TearDown.
  // Currently, using SetUp/TearDown causes VerifySlave* test to
  // fail with a duplicate slave id message. However, everything
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

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  // Start a mock slave since we aren't testing the slave hooks yet.
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

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // Add label which will be removed by the hook.
  Labels* labels = task.mutable_labels();
  labels->add_labels()->CopyFrom(createLabel(
        testRemoveLabelKey, testRemoveLabelValue));

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<RunTaskMessage> runTaskMessage =
    FUTURE_PROTOBUF(RunTaskMessage(), _, _);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(runTaskMessage);

  AWAIT_READY(status);

  // At launchTasks, the label decorator hook inside should have been
  // executed and we should see the labels now. Also, verify that the
  // hook module has stripped the first 'testRemoveLabelKey' label.
  // We do this by ensuring that only one label is present and that it
  // is the new 'testLabelKey' label.
  const Labels &labels_ = runTaskMessage.get().task().labels();
  ASSERT_EQ(1, labels_.labels_size());

  EXPECT_EQ(labels_.labels().Get(0).key(), testLabelKey);
  EXPECT_EQ(labels_.labels().Get(0).value(), testLabelValue);

  driver.stop();
  driver.join();

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
// for slave. We expect the environment-decorator hook to create a
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

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Executor shutdown would force the Slave to execute the
  // remove-executor hook.
  EXPECT_CALL(exec, shutdown(_));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  // On successful completion of the "slaveLaunchExecutorHook", the
  // test hook will send a HookExecuted message to itself. We wait
  // until that message is intercepted by the testing infrastructure.
  Future<HookExecuted> hookFuture = FUTURE_PROTOBUF(HookExecuted(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);

  driver.stop();
  driver.join();

  // The scheduler shutdown from above forces the executor to
  // shutdown. This in turn should force the Slave to execute
  // the remove-executor hook.
  // Here, we wait for the hook to finish execution.
  AWAIT_READY(hookFuture);

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that the slave run task label decorator can add
// and remove labels from a task during the launch sequence. A task
// with two labels ("foo":"bar" and "bar":"baz") is launched and will
// get modified by the slave hook to strip the "foo":"bar" pair and
// add a new "baz":"qux" pair.
TEST_F(HookTest, VerifySlaveRunTaskHook)
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // Add two labels: (1) will be removed by the hook to ensure that
  // runTaskHook can remove labels (2) will be preserved to ensure
  // that the framework can add labels to the task and have those be
  // available by the end of the launch task sequence when hooks are
  // used (to protect against hooks removing labels completely).
  Labels* labels = task.mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<TaskInfo> taskInfo;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&taskInfo),
        SendStatusUpdateFromTask(TASK_RUNNING)));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(taskInfo);

  // The master hook will hang an extra label off.
  const Labels& labels_ = taskInfo.get().labels();

  ASSERT_EQ(3, labels_.labels_size());

  // The slave run task hook will prepend a new "baz":"qux" label.
  EXPECT_EQ(labels_.labels(0).key(), "baz");
  EXPECT_EQ(labels_.labels(0).value(), "qux");

  // Master launch task hook will still hang off test label.
  EXPECT_EQ(labels_.labels(1).key(), testLabelKey);
  EXPECT_EQ(labels_.labels(1).value(), testLabelValue);

  // And lastly, we only expect the "foo":"bar" pair to be stripped by
  // the module. The last pair should be the original "bar":"baz"
  // pair set by the test.
  EXPECT_EQ(labels_.labels(2).key(), "bar");
  EXPECT_EQ(labels_.labels(2).value(), "baz");

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that the slave task status label decorator can
// add and remove labels from a TaskStatus during the status update
// sequence. A TaskStatus with two labels ("foo":"bar" and
// "bar":"baz") is sent from the executor. The labels get modified by
// the slave hook to strip the "foo":"bar" pair and/ add a new
// "baz":"qux" pair.
TEST_F(HookTest, VerifySlaveTaskStatusLabelDecorator)
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers.get().size());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(execTask);

  // Now send TASK_RUNNING update with two labels. The first label
  // ("foo:bar") will be removed by the task status hook to ensure
  // that it can remove labels. The second label will be preserved
  // and forwarded to Master (and eventually to the framework).
  // The hook also adds a new label with the same key but a different
  // value ("bar:quz").
  TaskStatus runningStatus;
  runningStatus.mutable_task_id()->MergeFrom(execTask.get().task_id());
  runningStatus.set_state(TASK_RUNNING);

  // Add two labels to the TaskStatus
  Labels* labels = runningStatus.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));

  execDriver->sendStatusUpdate(runningStatus);

  AWAIT_READY(status);

  // The master hook will hang an extra label off.
  const Labels& labels_ = status.get().labels();

  EXPECT_EQ(2, labels_.labels_size());

  // The test hook will prepend a new "baz":"qux" label.
  EXPECT_EQ(labels_.labels(0).key(), "bar");
  EXPECT_EQ(labels_.labels(0).value(), "qux");

  // And lastly, we only expect the "foo":"bar" pair to be stripped by
  // the module. The last pair should be the original "bar":"baz"
  // pair set by the test.
  EXPECT_EQ(labels_.labels(1).key(), "bar");
  EXPECT_EQ(labels_.labels(1).value(), "baz");

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
