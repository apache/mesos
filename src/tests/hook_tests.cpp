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

#include <mesos/module.hpp>

#include <mesos/slave/container_logger.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "common/protobuf_utils.hpp"

#include "hook/manager.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"
#include "tests/resources_utils.hpp"

using namespace mesos::modules;

using mesos::internal::master::Master;

using mesos::internal::protobuf::createLabel;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::executorEnvironment;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerLogger;
using mesos::slave::ContainerTermination;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Shared;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
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
const char* testErrorLabelKey = "MESOS_Test_Error_Label";
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

  ~HookTest() override
  {
    // Unload the hooks so a subsequent install may succeed.
    EXPECT_SOME(HookManager::unload(HOOK_MODULE_NAME));
  }
};


// Test varioud hook install/uninstall mechanisms.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, HookLoading)
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
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HookTest,
    VerifyMasterLaunchTaskLabelDecoratorHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock slave since we aren't testing the slave hooks yet.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

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
  ASSERT_FALSE(offers->empty());

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

  Future<RunTaskMessage> runTaskMessage =
    FUTURE_PROTOBUF(RunTaskMessage(), _, _);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(runTaskMessage);

  AWAIT_READY(status);

  // At launchTasks, the label decorator hook inside should have been
  // executed and we should see the labels now. Also, verify that the
  // hook module has stripped the first 'testRemoveLabelKey' label.
  // We do this by ensuring that only one label is present and that it
  // is the new 'testLabelKey' label.
  const Labels &labels_ = runTaskMessage->task().labels();
  ASSERT_EQ(1, labels_.labels_size());

  EXPECT_EQ(testLabelKey, labels_.labels().Get(0).key());
  EXPECT_EQ(testLabelValue, labels_.labels().Get(0).value());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Test that the resource decorator hook implicitly injects a custom resource
// in taskinfo message during master launch task.
//
// Concretely, the agent declares 100Mbps of network bandwidth and the
// framework does not consume any. Consequently the hook will set 10Mbps as a
// default value for the task.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HookTest,
    VerifyMasterLaunchTaskResourceDecoratorHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources.get() += ";network_bandwidth:100";
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

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
  ASSERT_FALSE(offers->empty());

  // Extract all resources from the offer except network bandwidth.
  Resources resourcesWithoutNetworkBandwidth = Resources(
    offers.get()[0].resources()).filter(
      [](const Resource& r) {
        return r.name() != "network_bandwidth";
      });

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(resourcesWithoutNetworkBandwidth);
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  Future<RunTaskMessage> runTaskMessage =
    FUTURE_PROTOBUF(RunTaskMessage(), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(runTaskMessage);

  AWAIT_READY(status);

  const Resources& taskResources = runTaskMessage->task().resources();
  Option<Value::Scalar> value = taskResources
    .get<Value::Scalar>("network_bandwidth");

  ASSERT_SOME(value);
  EXPECT_EQ(10, value->value());

  driver.stop();
  driver.join();
}


// This test forces a `SlaveLost` event. When this happens, we expect the
// `masterSlaveLostHook` to be invoked and await an internal libprocess event
// to trigger in the module code.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, MasterSlaveLostHookTest)
{
  Future<HookExecuted> hookFuture = FUTURE_PROTOBUF(HookExecuted(), _, _);

  DROP_PROTOBUFS(PingSlaveMessage(), _, _);

  master::Flags masterFlags = CreateMasterFlags();

  // Speed up timeout cycles.
  masterFlags.agent_ping_timeout = Seconds(1);
  masterFlags.max_agent_ping_timeouts = 1;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock Agent since we aren't testing the slave hooks.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Make sure Agent is up and running.
  AWAIT_READY(slaveRegisteredMessage);

  // Forward clock slave timeout.
  Duration totalTimeout =
    masterFlags.agent_ping_timeout * masterFlags.max_agent_ping_timeouts;

  Clock::pause();
  Clock::advance(totalTimeout);
  Clock::settle();
  Clock::resume();

  // `masterSlaveLostHook()` should be called from within module code.
  AWAIT_READY(hookFuture);

  // TODO(nnielsen): Verify hook signal type.
}


// Test that the environment decorator hook adds a new environment
// variable to the executor runtime.
// Test hook adds a new environment variable "FOO" to the executor
// with a value "bar". We validate the hook by verifying the value
// of this environment variable.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HookTest,
    VerifySlaveExecutorEnvironmentDecorator)
{
  const string& directory = os::getcwd(); // We're inside a temporary sandbox.

  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value("test_container");

  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "test $FOO = 'bar'");

  SlaveID slaveId = SlaveID();

  std::map<string, string> environment = executorEnvironment(
      flags,
      executorInfo,
      directory,
      slaveId,
      PID<Slave>(),
      None(),
      false);

  // Test hook adds a new environment variable "FOO" to the executor
  // with a value "bar". A '0' (success) exit status for the following
  // command validates the hook.
  process::Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, directory),
      environment,
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the container.
  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());
}


// Test executor environment decorator hook and remove executor hook
// for slave. We expect the environment-decorator hook to create a
// temporary file and the remove-executor hook to delete that file.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, VerifySlaveLaunchExecutorHook)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

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
  ASSERT_FALSE(offers->empty());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Executor shutdown would force the Slave to execute the
  // remove-executor hook.
  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());

  // On successful completion of the "slaveLaunchExecutorHook", the
  // test hook will send a HookExecuted message to itself. We wait
  // until that message is intercepted by the testing infrastructure.
  Future<HookExecuted> hookFuture = FUTURE_PROTOBUF(HookExecuted(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  driver.stop();
  driver.join();

  // Explicitly destroy the container.
  AWAIT_READY(shutdown);
  containerizer.destroy(offers.get()[0].framework_id(), DEFAULT_EXECUTOR_ID);

  // The scheduler shutdown from above forces the executor to
  // shutdown. This in turn should force the Slave to execute
  // the remove-executor hook.
  // Here, we wait for the hook to finish execution.
  AWAIT_READY(hookFuture);
}


// This test verifies that the slave run task label decorator can add
// and remove labels from a task during the launch sequence. A task
// with two labels ("foo":"bar" and "bar":"baz") is launched and will
// get modified by the slave hook to strip the "foo":"bar" pair and
// add a new "baz":"qux" pair.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, VerifySlaveRunTaskHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

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

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<TaskInfo> taskInfo;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&taskInfo),
        SendStatusUpdateFromTask(TASK_RUNNING)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(taskInfo);

  // The master hook will hang an extra label off.
  const Labels& labels_ = taskInfo->labels();

  ASSERT_EQ(3, labels_.labels_size());

  // The slave run task hook will prepend a new "baz":"qux" label.
  EXPECT_EQ("baz", labels_.labels(0).key());
  EXPECT_EQ("qux", labels_.labels(0).value());

  // Master launch task hook will still hang off test label.
  EXPECT_EQ(testLabelKey, labels_.labels(1).key());
  EXPECT_EQ(testLabelValue, labels_.labels(1).value());

  // And lastly, we only expect the "foo":"bar" pair to be stripped by
  // the module. The last pair should be the original "bar":"baz"
  // pair set by the test.
  EXPECT_EQ("bar", labels_.labels(2).key());
  EXPECT_EQ("baz", labels_.labels(2).value());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the slave task status label decorator can
// add and remove labels from a TaskStatus during the status update
// sequence. A TaskStatus with two labels ("foo":"bar" and
// "bar":"baz") is sent from the executor. The labels get modified by
// the slave hook to strip the "foo":"bar" pair and/ add a new
// "baz":"qux" pair.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, VerifySlaveTaskStatusDecorator)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(execTask);

  // Now send TASK_RUNNING update with two labels. The first label
  // ("foo:bar") will be removed by the task status hook to ensure
  // that it can remove labels. The second label will be preserved
  // and forwarded to Master (and eventually to the framework).
  // The hook also adds a new label with the same key but a different
  // value ("bar:quz").
  TaskStatus runningStatus;
  runningStatus.mutable_task_id()->MergeFrom(execTask->task_id());
  runningStatus.set_state(TASK_RUNNING);

  // Add two labels to the TaskStatus
  Labels* labels = runningStatus.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));

  execDriver->sendStatusUpdate(runningStatus);

  AWAIT_READY(status);

  // The hook will hang an extra label off.
  const Labels& labels_ = status->labels();

  EXPECT_EQ(2, labels_.labels_size());

  // The test hook will prepend a new "baz":"qux" label.
  EXPECT_EQ("bar", labels_.labels(0).key());
  EXPECT_EQ("qux", labels_.labels(0).value());

  // And lastly, we only expect the "foo":"bar" pair to be stripped by
  // the module. The last pair should be the original "bar":"baz"
  // pair set by the test.
  EXPECT_EQ("bar", labels_.labels(1).key());
  EXPECT_EQ("baz", labels_.labels(1).value());

  // Now validate TaskInfo.container_status. We must have received a
  // container_status with one network_info set by the test hook module.
  EXPECT_TRUE(status->has_container_status());
  EXPECT_EQ(1, status->container_status().network_infos().size());

  const NetworkInfo networkInfo =
    status->container_status().network_infos(0);

  // The hook module sets up '4.3.2.1' as the IP address and 'public' as the
  // network isolation group. The `ip_address` field is deprecated, but the
  // hook module should continue to set it as well as the new `ip_addresses`
  // field for now.
  EXPECT_EQ(1, networkInfo.ip_addresses().size());
  EXPECT_TRUE(networkInfo.ip_addresses(0).has_ip_address());
  EXPECT_EQ("4.3.2.1", networkInfo.ip_addresses(0).ip_address());

  EXPECT_EQ(1, networkInfo.groups().size());
  EXPECT_EQ("public", networkInfo.groups(0));

  EXPECT_TRUE(networkInfo.has_labels());
  EXPECT_EQ(1, networkInfo.labels().labels().size());

  const Label networkInfoLabel = networkInfo.labels().labels(0);

  // Finally, the labels set inside NetworkInfo by the hook module.
  EXPECT_EQ("net_foo", networkInfoLabel.key());
  EXPECT_EQ("net_bar", networkInfoLabel.value());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the slave pre-launch docker environment
// decorator can attach environment variables to a task exclusively.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  HookTest, ROOT_DOCKER_VerifySlavePreLaunchDockerTaskExecutorDecorator)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer containerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  // The task should see `HOOKTEST_TASK` but not `HOOKTEST_EXECUTOR`.
  TaskInfo task = createTask(
      offers.get()[0],
      "test \"$HOOKTEST_TASK\" = 'foo' && "
      "test ! \"$HOOKTEST_EXECUTOR\" = 'bar'");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  Future<vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// This test verifies that the slave pre-launch docker validator hook can check
// labels on a task and subsequently prevent the task from being launched
// if a specific label is present.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  HookTest, ROOT_DOCKER_VerifySlavePreLaunchDockerValidator)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer containerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());

  // Add a special label which the validator hook checks for.
  // The existence of this label will cause the hook to reject the task.
  Labels* labels = task.mutable_labels();
  labels->add_labels()->CopyFrom(
      createLabel(testErrorLabelKey, testLabelValue));

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  CommandInfo command;
  command.set_value("exit 0");

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<TaskStatus> statusError;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusError));

  AWAIT_READY(statusError);

  EXPECT_EQ(TASK_FAILED, statusError->state());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED, statusError->reason());

  driver.stop();
  driver.join();
}


// Test that the prepare launch docker hook execute before launch
// a docker container. Test hook create a file "foo" in the sandbox
// directory. When the docker container launched, the sandbox directory
// is mounted to the docker container. We validate the hook by verifying
// the "foo" file exists in the docker container or not.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  HookTest, ROOT_DOCKER_VerifySlavePreLaunchDockerHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer containerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("test -f " + path::join(flags.sandbox_directory, "foo"));

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  Future<vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Test that the slave post fetch hook is executed after fetching the
// URIs but before the container is launched. We launch a command task
// with a file URI (file name is "post_fetch_hook"). The test hook
// will try to delete that file in the sandbox directory. We validate
// the hook by verifying that "post_fetch_hook" file does not exist in
// the sandbox when container is running.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HookTest, ROOT_DOCKER_VerifySlavePostFetchHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<Owned<Docker>> _docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket);
  ASSERT_SOME(_docker);

  Shared<Docker> docker = _docker->share();

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  DockerContainerizer containerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      &containerizer,
      flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "test ! -f " + path::join(flags.sandbox_directory, "post_fetch_hook"));

  // Add a URI for a file on the host filesystem. This file will be
  // fetched to the sandbox and will later be deleted by the hook.
  const string file = path::join(sandbox.get(), "post_fetch_hook");
  ASSERT_SOME(os::touch(file));

  CommandInfo::URI* uri = task.mutable_command()->add_uris();
  uri->set_value(file);

  ContainerInfo* containerInfo = task.mutable_container();
  containerInfo->set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo* dockerInfo = containerInfo->mutable_docker();
  dockerInfo->set_image("alpine");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  Future<vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// TODO(jieyu): Add a test for slavePostFetchHook using Mesos
// containerizer.


// Test that the changes made by the resources decorator hook are correctly
// propagated to the resource offer.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HookTest, VerifySlaveResourcesAndAttributesDecorator)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock slave since we aren't testing the slave hooks yet.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

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
  ASSERT_FALSE(offers->empty());

  Resources resources = offers.get()[0].resources();

  // The test hook sets "cpus" to 4 and adds a resource named
  // "foo" of type set with values "bar" and "baz".
  //
  // TODO(bmahler): Avoid the need for non-local reasoning here
  // about the test hook. E.g. Expose the resources from the test
  // hook and use them here.
  const size_t TEST_HOOK_CPUS = 4;
  const Resources TEST_HOOK_ADDITIONAL_RESOURCES =
    Resources::parse("foo:{bar,baz}").get();

  EXPECT_EQ(TEST_HOOK_CPUS, resources.cpus().get());

  const string allocationRole = DEFAULT_FRAMEWORK_INFO.roles(0);
  EXPECT_TRUE(resources.contains(
      allocatedResources(TEST_HOOK_ADDITIONAL_RESOURCES, allocationRole)));

  // The test hook does not modify "mem", the default value must still be
  // present.
  EXPECT_SOME(resources.mem());

  // The test hook adds an attribute named "rack" with value "rack1".
  Attributes attributes = offers.get()[0].attributes();
  ASSERT_EQ(attributes.get(0).name(), "rack");
  ASSERT_EQ(attributes.get(0).text().value(), "rack1");

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
