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
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/net.hpp>
#include <stout/strings.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launcher.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/isolator.hpp"
#include "tests/containerizer/launcher.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::Slave;

using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::RunState;
using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using std::list;
using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class MesosContainerizerIsolatorPreparationTest :
  public TemporaryDirectoryTest
{
public:
  // Construct a MesosContainerizer with TestIsolator(s) which use the provided
  // 'prepare' command(s).
  Try<Owned<MesosContainerizer>> CreateContainerizer(
      Fetcher* fetcher,
      const vector<Option<ContainerLaunchInfo>>& launchInfos)
  {
    vector<Owned<Isolator>> isolators;

    foreach (const Option<ContainerLaunchInfo>& launchInfo, launchInfos) {
      Try<Isolator*> isolator = TestIsolatorProcess::create(launchInfo);
      if (isolator.isError()) {
        return Error(isolator.error());
      }

      isolators.push_back(Owned<Isolator>(isolator.get()));
    }

    slave::Flags flags;
    flags.launcher_dir = getLauncherDir();

    Try<Launcher*> launcher = PosixLauncher::create(flags);
    if (launcher.isError()) {
      return Error(launcher.error());
    }

    // Create and initialize a new container logger.
    Try<ContainerLogger*> logger =
      ContainerLogger::create(flags.container_logger);

    if (logger.isError()) {
      return Error("Failed to create container logger: " + logger.error());
    }

    Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
    if (provisioner.isError()) {
      return Error("Failed to create provisioner: " + provisioner.error());
    }

    return Owned<MesosContainerizer>(new MesosContainerizer(
        flags,
        false,
        fetcher,
        Owned<ContainerLogger>(logger.get()),
        Owned<Launcher>(launcher.get()),
        provisioner.get(),
        isolators));
  }

  Try<Owned<MesosContainerizer>> CreateContainerizer(
      Fetcher* fetcher,
      const Option<ContainerLaunchInfo>& launchInfo)
  {
    vector<Option<ContainerLaunchInfo>> launchInfos;
    launchInfos.push_back(launchInfo);

    return CreateContainerizer(fetcher, launchInfos);
  }
};


// The isolator has a prepare command that succeeds.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptSucceeds)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file = path::join(directory, "child.script.executed");

  Fetcher fetcher;

  ContainerLaunchInfo launchInfo;
  launchInfo.add_commands()->set_value("touch " + file);

  Try<Owned<MesosContainerizer>> containerizer = CreateContainerizer(
      &fetcher,
      launchInfo);
  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait until the launch completes.
  AWAIT_READY(launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the child exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));

  // Destroy the container.
  containerizer.get()->destroy(containerId);
}


// The isolator has a prepare command that fails.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptFails)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file = path::join(directory, "child.script.executed");

  Fetcher fetcher;

  ContainerLaunchInfo launchInfo;
  launchInfo.add_commands()->set_value("touch " + file + " && exit 1");

  Try<Owned<MesosContainerizer>> containerizer = CreateContainerizer(
      &fetcher,
      launchInfo);
  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait until the launch completes.
  AWAIT_READY(launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the child failed to exit correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_NE(0, wait.get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));

  // Destroy the container.
  containerizer.get()->destroy(containerId);
}


// There are two isolators, one with a prepare command that succeeds
// and another that fails. The execution order is not defined but the
// launch should fail from the failing prepare command.
TEST_F(MesosContainerizerIsolatorPreparationTest, MultipleScripts)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file1 = path::join(directory, "child.script.executed.1");
  string file2 = path::join(directory, "child.script.executed.2");

  vector<Option<ContainerLaunchInfo>> launchInfos;

  // This isolator prepare command one will succeed if called first, otherwise
  // it won't get run.
  ContainerLaunchInfo launch1;
  launch1.add_commands()->set_value("touch " + file1 + " && exit 0");
  launchInfos.push_back(launch1);

  // This will fail, either first or after the successful command.
  ContainerLaunchInfo launch2;
  launch2.add_commands()->set_value("touch " + file2 + " && exit 1");
  launchInfos.push_back(launch2);

  Fetcher fetcher;

  Try<Owned<MesosContainerizer>> containerizer =
    CreateContainerizer(&fetcher, launchInfos);

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait until the launch completes.
  AWAIT_READY(launch);

  // Wait for the child (preparation script(s) + executor) to complete.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);
  AWAIT_READY(wait);

  // Check the child failed to exit correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_NE(0, wait.get().status());

  // Check the failing preparation script has actually ran.
  EXPECT_TRUE(os::exists(file2));

  // Destroy the container.
  containerizer.get()->destroy(containerId);
}


// The isolator sets an environment variable for the Executor. The
// Executor then creates a file as pointed to by environment
// variable. Finally, after the executor has terminated, we check for
// the existence of the file.
TEST_F(MesosContainerizerIsolatorPreparationTest, ExecutorEnvironmentVariable)
{
  // Set LIBPROCESS_IP so that we can test if it gets passed to the executor.
  Option<string> libprocessIP = os::getenv("LIBPROCESS_IP");
  net::IP ip = net::IP(INADDR_LOOPBACK);
  os::setenv("LIBPROCESS_IP", stringify(ip));

  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file = path::join(directory, "child.script.executed");

  Fetcher fetcher;

  ContainerLaunchInfo launchInfo;

  Environment::Variable* variable =
    launchInfo.mutable_environment()->add_variables();

  variable->set_name("TEST_ENVIRONMENT");
  variable->set_value(file);

  Try<Owned<MesosContainerizer>> containerizer = CreateContainerizer(
      &fetcher,
      launchInfo);

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  // Ensure that LIBPROCESS_IP has been passed despite the explicit
  // specification of the environment. If so, then touch the test file.
  string executorCmd =
    "if [ -n \"$LIBPROCESS_IP\" ]; "
    "then touch $TEST_ENVIRONMENT; fi";

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", executorCmd),
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait until the launch completes.
  AWAIT_READY(launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the child exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));

  // Destroy the container.
  containerizer.get()->destroy(containerId);

  // Reset LIBPROCESS_IP if necessary.
  if (libprocessIP.isSome()) {
    os::setenv("LIBPROCESS_IP", libprocessIP.get());
  } else {
    os::unsetenv("LIBPROCESS_IP");
  }
}


class MesosContainerizerExecuteTest : public TemporaryDirectoryTest {};


TEST_F(MesosContainerizerExecuteTest, IoRedirection)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;

  // Use local=false so std{err,out} are redirected to files.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value("test_container");

  string errMsg = "this is stderr";
  string outMsg = "this is stdout";
  string command =
    "(echo '" + errMsg + "' 1>&2) && echo '" + outMsg + "'";

  Future<bool> launch = containerizer->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", command),
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  // Check that std{err, out} was redirected.
  // NOTE: Fetcher uses GLOG, which outputs extra information to
  // stderr.
  Try<string> stderr = os::read(path::join(directory, "stderr"));
  ASSERT_SOME(stderr);
  EXPECT_TRUE(strings::contains(stderr.get(), errMsg));

  EXPECT_SOME_EQ(outMsg + "\n", os::read(path::join(directory, "stdout")));
}


class MesosContainerizerDestroyTest : public MesosTest {};


class MockMesosContainerizerProcess : public MesosContainerizerProcess
{
public:
  MockMesosContainerizerProcess(
      const slave::Flags& flags,
      bool local,
      Fetcher* fetcher,
      const Owned<ContainerLogger>& logger,
      const Owned<Launcher>& launcher,
      const Owned<Provisioner>& provisioner,
      const vector<Owned<Isolator>>& isolators)
    : MesosContainerizerProcess(
          flags,
          local,
          fetcher,
          logger,
          launcher,
          provisioner,
          isolators)
  {
    // NOTE: See TestContainerizer::setup for why we use
    // 'EXPECT_CALL' and 'WillRepeatedly' here instead of
    // 'ON_CALL' and 'WillByDefault'.
    EXPECT_CALL(*this, exec(_, _))
      .WillRepeatedly(Invoke(this, &MockMesosContainerizerProcess::_exec));
  }

  MOCK_METHOD2(
      exec,
      Future<bool>(
          const ContainerID& containerId,
          int pipeWrite));

  Future<bool> _exec(
      const ContainerID& containerId,
      int pipeWrite)
  {
    return MesosContainerizerProcess::exec(
        containerId,
        pipeWrite);
  }
};


class MockIsolator : public mesos::slave::Isolator
{
public:
  MockIsolator()
  {
    EXPECT_CALL(*this, watch(_))
      .WillRepeatedly(Return(watchPromise.future()));

    EXPECT_CALL(*this, isolate(_, _))
      .WillRepeatedly(Return(Nothing()));

    EXPECT_CALL(*this, cleanup(_))
      .WillRepeatedly(Return(Nothing()));

    EXPECT_CALL(*this, prepare(_, _))
      .WillRepeatedly(Invoke(this, &MockIsolator::_prepare));
  }

  MOCK_METHOD2(
      recover,
      Future<Nothing>(
          const list<ContainerState>&,
          const hashset<ContainerID>&));

  MOCK_METHOD2(
      prepare,
      Future<Option<ContainerLaunchInfo>>(
          const ContainerID&,
          const ContainerConfig&));

  virtual Future<Option<ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const ContainerConfig& containerConfig)
  {
    return None();
  }

  MOCK_METHOD2(
      isolate,
      Future<Nothing>(const ContainerID&, pid_t));

  MOCK_METHOD1(
      watch,
      Future<mesos::slave::ContainerLimitation>(const ContainerID&));

  MOCK_METHOD2(
      update,
      Future<Nothing>(const ContainerID&, const Resources&));

  MOCK_METHOD1(
      usage,
      Future<ResourceStatistics>(const ContainerID&));

  MOCK_METHOD1(
      cleanup,
      Future<Nothing>(const ContainerID&));

  Promise<mesos::slave::ContainerLimitation> watchPromise;
};


// Destroying a mesos containerizer while it is fetching should
// complete without waiting for the fetching to finish.
TEST_F(MesosContainerizerDestroyTest, DestroyWhileFetching)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<Launcher*> launcher = PosixLauncher::create(flags);
  ASSERT_SOME(launcher);

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  MockMesosContainerizerProcess* process = new MockMesosContainerizerProcess(
      flags,
      true,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      Owned<Launcher>(launcher.get()),
      provisioner.get(),
      vector<Owned<Isolator>>());

  Future<Nothing> exec;
  Promise<bool> promise;

  // Letting exec hang to simulate a long fetch.
  EXPECT_CALL(*process, exec(_, _))
    .WillOnce(DoAll(FutureSatisfy(&exec),
                    Return(promise.future())));

  MesosContainerizer containerizer((Owned<MesosContainerizerProcess>(process)));

  ContainerID containerId;
  containerId.set_value("test_container");

  TaskInfo taskInfo;
  CommandInfo commandInfo;
  taskInfo.mutable_command()->MergeFrom(commandInfo);

  containerizer.launch(
      containerId,
      taskInfo,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      os::getcwd(),
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  Future<containerizer::Termination> wait = containerizer.wait(containerId);

  AWAIT_READY(exec);

  containerizer.destroy(containerId);

  // The container should still exit even if fetch didn't complete.
  AWAIT_READY(wait);
}


// Destroying a mesos containerizer while it is preparing should wait
// until isolators are finished preparing before destroying.
TEST_F(MesosContainerizerDestroyTest, DestroyWhilePreparing)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<Launcher*> launcher = PosixLauncher::create(flags);
  ASSERT_SOME(launcher);

  MockIsolator* isolator = new MockIsolator();

  Future<Nothing> prepare;
  Promise<Option<ContainerLaunchInfo>> promise;

  // Simulate a long prepare from the isolator.
  EXPECT_CALL(*isolator, prepare(_, _))
    .WillOnce(DoAll(FutureSatisfy(&prepare),
                    Return(promise.future())));

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  MockMesosContainerizerProcess* process = new MockMesosContainerizerProcess(
      flags,
      true,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      Owned<Launcher>(launcher.get()),
      provisioner.get(),
      {Owned<Isolator>(isolator)});

  MesosContainerizer containerizer((Owned<MesosContainerizerProcess>(process)));

  ContainerID containerId;
  containerId.set_value("test_container");

  TaskInfo taskInfo;
  CommandInfo commandInfo;
  taskInfo.mutable_command()->MergeFrom(commandInfo);

  containerizer.launch(
      containerId,
      taskInfo,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      os::getcwd(),
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  Future<containerizer::Termination> wait = containerizer.wait(containerId);

  AWAIT_READY(prepare);

  containerizer.destroy(containerId);

  // The container should not exit until prepare is complete.
  ASSERT_TRUE(wait.isPending());

  // Need to help the compiler to disambiguate between overloads.
  ContainerLaunchInfo launchInfo;
  launchInfo.add_commands()->CopyFrom(commandInfo);
  Option<ContainerLaunchInfo> option = launchInfo;
  promise.set(option);

  AWAIT_READY(wait);

  containerizer::Termination termination = wait.get();

  EXPECT_EQ(
      "Container destroyed while preparing isolators",
      termination.message());

  EXPECT_FALSE(termination.has_status());
}


// This action destroys the container using the real launcher and
// waits until the destroy is complete.
ACTION_P(InvokeDestroyAndWait, launcher)
{
  Future<Nothing> destroy = launcher->real->destroy(arg0);
  AWAIT_READY(destroy);
}


// This test verifies that when a container destruction fails the
// 'container_destroy_errors' metric is updated.
TEST_F(MesosContainerizerDestroyTest, LauncherDestroyFailure)
{
  // Create a TestLauncher backed by PosixLauncher.
  slave::Flags flags = CreateSlaveFlags();

  Try<Launcher*> launcher_ = PosixLauncher::create(flags);
  ASSERT_SOME(launcher_);

  TestLauncher* launcher = new TestLauncher(Owned<Launcher>(launcher_.get()));

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  MesosContainerizerProcess* process = new MesosContainerizerProcess(
      flags,
      true,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      Owned<Launcher>(launcher),
      provisioner.get(),
      vector<Owned<Isolator>>());

  MesosContainerizer containerizer((Owned<MesosContainerizerProcess>(process)));

  ContainerID containerId;
  containerId.set_value("test_container");

  TaskInfo taskInfo;
  CommandInfo commandInfo;
  taskInfo.mutable_command()->MergeFrom(commandInfo);

  // Destroy the container using the PosixLauncher but return a failed
  // future to the containerizer.
  EXPECT_CALL(*launcher, destroy(_))
    .WillOnce(DoAll(InvokeDestroyAndWait(launcher),
                    Return(Failure("Destroy failure"))));

  Future<bool> launch = containerizer.launch(
      containerId,
      taskInfo,
      CREATE_EXECUTOR_INFO("executor", "sleep 1000"),
      os::getcwd(),
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  AWAIT_READY(launch);

  Future<containerizer::Termination> wait = containerizer.wait(containerId);

  containerizer.destroy(containerId);

  // The container destroy should fail.
  AWAIT_FAILED(wait);

  // We settle the clock here to ensure that the processing of
  // 'MesosContainerizerProcess::__destroy()' is complete and the
  // metric is updated.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Ensure that the metric is updated.
  JSON::Object metrics = Metrics();
  ASSERT_EQ(
      1u,
      metrics.values.count("containerizer/mesos/container_destroy_errors"));
  ASSERT_EQ(
      1u,
      metrics.values["containerizer/mesos/container_destroy_errors"]);
}


class MesosContainerizerRecoverTest : public MesosTest {};


// This test checks that MesosContainerizer doesn't recover executors
// that were started by another containerizer (e.g: Docker).
TEST_F(MesosContainerizerRecoverTest, SkipRecoverNonMesosContainers)
{
  slave::Flags flags = CreateSlaveFlags();
  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ExecutorID executorId;
  executorId.set_value(UUID::random().toString());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executorInfo;
  executorInfo.mutable_container()->set_type(ContainerInfo::DOCKER);

  ExecutorState executorState;
  executorState.info = executorInfo;
  executorState.latest = containerId;

  RunState runState;
  runState.id = containerId;
  executorState.runs.put(containerId, runState);

  FrameworkState frameworkState;
  frameworkState.executors.put(executorId, executorState);

  SlaveState slaveState;
  FrameworkID frameworkId;
  frameworkId.set_value(UUID::random().toString());
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = containerizer.get()->recover(slaveState);
  AWAIT_READY(recover);

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(0u, containers.get().size());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
