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
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/net.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launcher.hpp"

#include "slave/containerizer/mesos/provisioner/backend.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_fetcher.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/isolator.hpp"
#include "tests/containerizer/launcher.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Backend;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::executorEnvironment;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::FetcherProcess;
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::MESOS_CONTAINERIZER;
using mesos::internal::slave::SubprocessLauncher;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::ProvisionInfo;
using mesos::internal::slave::Slave;

using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::RunState;
using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;
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

class MesosContainerizerTest
  : public ContainerizerTest<slave::MesosContainerizer> {};


// TODO(benh): Parameterize this test by each `Launcher`.
TEST_F(MesosContainerizerTest, Launch)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "exit 42", "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());
}


TEST_F(MesosContainerizerTest, StandaloneLaunch)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          createCommandInfo("exit 42"),
          "cpus:1;mem:64",
          sandbox.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());
}


TEST_F(MesosContainerizerTest, Destroy)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "sleep 1000", "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that ContainerID is properly set in the
// ContainerStatus returned from 'status()' method.
TEST_F(MesosContainerizerTest, StatusWithContainerID)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "posix/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "sleep 1000", "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);

  EXPECT_EQ(containerId, status->container_id());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


class MesosContainerizerIsolatorPreparationTest : public MesosTest
{
public:
  // Construct a MesosContainerizer with MockIsolator(s) which return
  // the provided ContainerLaunchInfo for Isolator::prepare.
  Try<MesosContainerizer*> createContainerizer(
      const vector<Option<ContainerLaunchInfo>>& launchInfos)
  {
    vector<Owned<Isolator>> isolators;

    foreach (const Option<ContainerLaunchInfo>& launchInfo, launchInfos) {
      MockIsolator* isolator = new MockIsolator();

      EXPECT_CALL(*isolator, prepare(_, _))
        .WillOnce(Return(launchInfo));

      isolators.push_back(Owned<Isolator>(isolator));
    }

    slave::Flags flags = CreateSlaveFlags();
    flags.launcher = "posix";

    fetcher.reset(new Fetcher(flags));

    Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
    if (launcher_.isError()) {
      return Error(launcher_.error());
    }

    Owned<Launcher> launcher(launcher_.get());

    Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
    if (provisioner.isError()) {
      return Error("Failed to create provisioner: " + provisioner.error());
    }

    return MesosContainerizer::create(
        flags,
        true,
        fetcher.get(),
        nullptr,
        std::move(launcher),
        provisioner->share(),
        isolators);
  }

  Try<MesosContainerizer*> createContainerizer(
      const Option<ContainerLaunchInfo>& launchInfo)
  {
    vector<Option<ContainerLaunchInfo>> launchInfos = {launchInfo};
    return createContainerizer(launchInfos);
  }

private:
  Owned<Fetcher> fetcher;
};


// The isolator has a prepare command that succeeds.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptSucceeds)
{
  string file = path::join(sandbox.get(), "child.script.executed");

  ContainerLaunchInfo launchInfo;
  launchInfo.add_pre_exec_commands()->set_value("touch " + file);

  Try<MesosContainerizer*> create = createContainerizer(launchInfo);
  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "exit 0"),
          sandbox.get()),
      map<string, string>(),
      None());

  // Wait until the launch completes.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the child exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));
}


// The isolator has a prepare command that fails.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptFails)
{
  string file = path::join(sandbox.get(), "child.script.executed");

  ContainerLaunchInfo launchInfo;
  launchInfo.add_pre_exec_commands()->set_value(
      "touch " + file + " && exit 1");

  Try<MesosContainerizer*> create = createContainerizer(launchInfo);
  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "exit 0"),
          sandbox.get()),
      map<string, string>(),
      None());

  // Wait until the launch completes.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the child failed to exit correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_NE(0, wait->get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));
}


// There are two isolators, one with a prepare command that succeeds
// and another that fails. The execution order is not defined but the
// launch should fail from the failing prepare command.
TEST_F(MesosContainerizerIsolatorPreparationTest, MultipleScripts)
{
  string file1 = path::join(sandbox.get(), "child.script.executed.1");
  string file2 = path::join(sandbox.get(), "child.script.executed.2");

  vector<Option<ContainerLaunchInfo>> launchInfos;

  // This isolator prepare command one will succeed if called first, otherwise
  // it won't get run.
  ContainerLaunchInfo launch1;
  launch1.add_pre_exec_commands()->set_value("touch " + file1 + " && exit 0");
  launchInfos.push_back(launch1);

  // This will fail, either first or after the successful command.
  ContainerLaunchInfo launch2;
  launch2.add_pre_exec_commands()->set_value("touch " + file2 + " && exit 1");
  launchInfos.push_back(launch2);

  Try<MesosContainerizer*> create = createContainerizer(launchInfos);
  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", "exit 0"),
          sandbox.get()),
      map<string, string>(),
      None());

  // Wait until the launch completes.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the child (preparation script(s) + executor) to complete.
  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the child failed to exit correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_NE(0, wait->get().status());

  // Check the failing preparation script has actually ran.
  EXPECT_TRUE(os::exists(file2));
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

  string file = path::join(sandbox.get(), "child.script.executed");

  ContainerLaunchInfo launchInfo;

  mesos::Environment::Variable* variable =
    launchInfo.mutable_environment()->add_variables();

  variable->set_name("TEST_ENVIRONMENT");
  variable->set_value(file);

  Try<MesosContainerizer*> create = createContainerizer(launchInfo);
  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Ensure that LIBPROCESS_IP has been passed despite the explicit
  // specification of the environment. If so, then touch the test file.
  string executorCmd =
    "if [ -n \"$LIBPROCESS_IP\" ]; "
    "then touch $TEST_ENVIRONMENT; fi";

  ExecutorInfo executorInfo = createExecutorInfo("executor", executorCmd);
  SlaveID slaveId = SlaveID();

  slave::Flags flags;

  map<string, string> environment = executorEnvironment(
      flags,
      executorInfo,
      sandbox.get(),
      slaveId,
      PID<Slave>(),
      None(),
      false);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, sandbox.get()),
      environment,
      None());

  // Wait until the launch completes.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the child (preparation script + executor) to complete.
  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the child exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));

  // Reset LIBPROCESS_IP if necessary.
  if (libprocessIP.isSome()) {
    os::setenv("LIBPROCESS_IP", libprocessIP.get());
  } else {
    os::unsetenv("LIBPROCESS_IP");
  }
}


class MesosContainerizerExecuteTest : public MesosTest {};


TEST_F(MesosContainerizerExecuteTest, IoRedirection)
{
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  // Use local=false so std{err,out} are redirected to files.
  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string errMsg = "this is stderr";
  string outMsg = "this is stdout";
  string command =
    "(echo '" + errMsg + "' 1>&2) && echo '" + outMsg + "'";

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo("executor", command),
          sandbox.get()),
      map<string, string>(),
      None());

  // Wait for the launch to complete.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the container.
  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that std{err, out} was redirected.
  // NOTE: Fetcher uses GLOG, which outputs extra information to
  // stderr.
  Try<string> stderr = os::read(path::join(sandbox.get(), "stderr"));
  ASSERT_SOME(stderr);
  EXPECT_TRUE(strings::contains(stderr.get(), errMsg));

  EXPECT_SOME_EQ(outMsg + "\n", os::read(path::join(sandbox.get(), "stdout")));
}


// This test verified that the stdout and stderr files in the task's sandbox
// are owned by the task user.
TEST_F(MesosContainerizerExecuteTest,
       ROOT_UNPRIVILEGED_USER_SandboxFileOwnership)
{
  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  ExecutorInfo executor = createExecutorInfo("executor", "exit 0");
  executor.mutable_command()->set_user(user.get());

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, sandbox.get()),
      map<string, string>(),
      None());

  // Wait for the launch to complete.
  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Result<uid_t> uid = os::getuid(user.get());
  ASSERT_SOME(uid);

  // Verify that stdout is owned by the task user.
  struct stat s;
  string stdoutPath = path::join(sandbox.get(), "stdout");
  EXPECT_EQ(0, ::stat(stdoutPath.c_str(), &s));
  EXPECT_EQ(uid.get(), s.st_uid);

  // Verify that stderr is owned by the task user.
  string stderrPath = path::join(sandbox.get(), "stderr");
  EXPECT_EQ(0, ::stat(stderrPath.c_str(), &s));
  EXPECT_EQ(uid.get(), s.st_uid);

  // Wait on the container.
  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);
  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());
}


class MesosContainerizerDestroyTest : public MesosTest {};


// Destroying a mesos containerizer while it is fetching should
// complete without waiting for the fetching to finish.
TEST_F(MesosContainerizerDestroyTest, DestroyWhileFetching)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  Owned<Launcher> launcher(launcher_.get());

  MockFetcherProcess* mockFetcherProcess = new MockFetcherProcess(flags);
  Owned<FetcherProcess> fetcherProcess(mockFetcherProcess);
  Fetcher fetcher(fetcherProcess);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      provisioner->share(),
      vector<Owned<Isolator>>());

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Future<Nothing> fetch;
  Promise<Nothing> promise;

  // Letting exec hang to simulate a long fetch.
  EXPECT_CALL(*mockFetcherProcess, _fetch(_, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(promise.future())));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");

  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());

  containerizer->launch(
      containerId,
      createContainerConfig(
          taskInfo,
          createExecutorInfo("executor", "exit 0"),
          sandbox.get()),
      map<string, string>(),
      None());

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);

  AWAIT_READY(fetch);

  containerizer->destroy(containerId);

  // The container should still exit even if fetch didn't complete.
  AWAIT_READY(wait);
  EXPECT_SOME(wait.get());
}


// Destroying a mesos containerizer while it is preparing should wait
// until isolators are finished preparing before destroying.
TEST_F(MesosContainerizerDestroyTest, DestroyWhilePreparing)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  Owned<Launcher> launcher(launcher_.get());

  MockIsolator* isolator = new MockIsolator();

  Future<Nothing> prepare;
  Promise<Option<ContainerLaunchInfo>> promise;

  // Simulate a long prepare from the isolator.
  EXPECT_CALL(*isolator, prepare(_, _))
    .WillOnce(DoAll(FutureSatisfy(&prepare),
                    Return(promise.future())));

  Fetcher fetcher(flags);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      provisioner->share(),
      {Owned<Isolator>(isolator)});

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");

  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());

  containerizer->launch(
      containerId,
      createContainerConfig(
          taskInfo,
          createExecutorInfo("executor", "exit 0"),
          sandbox.get()),
      map<string, string>(),
      None());

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);

  AWAIT_READY(prepare);

  containerizer->destroy(containerId);

  // The container should not exit until prepare is complete.
  ASSERT_TRUE(wait.isPending());

  // Need to help the compiler to disambiguate between overloads.
  ContainerLaunchInfo launchInfo;
  launchInfo.add_pre_exec_commands()->CopyFrom(taskInfo.command());
  Option<ContainerLaunchInfo> option = launchInfo;
  promise.set(option);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  ContainerTermination termination = wait->get();

  EXPECT_FALSE(termination.has_status());
}


// Ensures the containerizer responds correctly (false Future) to
// a request to destroy an unknown container.
TEST_F(MesosContainerizerDestroyTest, DestroyUnknownContainer)
{
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Option<ContainerTermination>> destroyed =
    containerizer->destroy(containerId);

  AWAIT_READY(destroyed);
  EXPECT_NONE(destroyed.get());
}


class MesosContainerizerProvisionerTest : public MesosTest {};


class MockProvisioner : public mesos::internal::slave::Provisioner
{
public:
  MockProvisioner() {}
  MOCK_CONST_METHOD1(recover,
                     Future<Nothing>(const hashset<ContainerID>&));

  MOCK_CONST_METHOD2(provision,
                     Future<mesos::internal::slave::ProvisionInfo>(
                         const ContainerID&,
                         const Image&));

  MOCK_CONST_METHOD1(destroy, Future<bool>(const ContainerID&));
};


// This test verifies that when the provision fails, the containerizer
// can be destroyed successfully.
TEST_F(MesosContainerizerProvisionerTest, ProvisionFailed)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  Owned<Launcher> launcher(new TestLauncher(Owned<Launcher>(launcher_.get())));

  MockProvisioner* provisioner = new MockProvisioner();

  Future<Nothing> provision;

  // Simulate a provision failure.
  EXPECT_CALL(*provisioner, provision(_, _))
    .WillOnce(DoAll(FutureSatisfy(&provision),
                    Return(Failure("provision failure"))));

  EXPECT_CALL(*provisioner, destroy(_))
    .WillOnce(Return(true));

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      Shared<Provisioner>(provisioner),
      vector<Owned<Isolator>>());

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Image image;
  image.set_type(Image::DOCKER);
  Image::Docker dockerImage;
  dockerImage.set_name(id::UUID::random().toString());
  image.mutable_docker()->CopyFrom(dockerImage);

  ContainerInfo::MesosInfo mesosInfo;
  mesosInfo.mutable_image()->CopyFrom(image);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->CopyFrom(mesosInfo);

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");

  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());

  taskInfo.mutable_container()->CopyFrom(containerInfo);

  ExecutorInfo executorInfo = createExecutorInfo("executor", "exit 0");
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(taskInfo, executorInfo, sandbox.get()),
      map<string, string>(),
      None());

  AWAIT_READY(provision);

  AWAIT_FAILED(launch);

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());

  EXPECT_FALSE(termination->get().has_status());
}


class MockBackend : public Backend
{
public:
  MockBackend() {}
  ~MockBackend() override {}

  MOCK_METHOD3(
      provision,
      Future<Option<vector<Path>>>(
          const vector<string>&,
          const string&,
          const string&));

  MOCK_METHOD2(
      destroy,
      Future<bool>(
          const string&,
          const string&));
};


// This test verifies that when the containerizer destroys a container while
// provisioner backend is provisioning rootfs for the container, backend
// destroy will not be invoked until backend provision finishes.
TEST_F(
    MesosContainerizerProvisionerTest,
    ROOT_INTERNET_CURL_DestroyWhileProvisioning)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";
  flags.isolation = "docker/runtime";
  flags.image_providers = "docker";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  Owned<Launcher> launcher(new TestLauncher(Owned<Launcher>(launcher_.get())));

  const string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);
  CHECK_SOME(os::mkdir(provisionerDir));
  CHECK_SOME(os::realpath(provisionerDir));

  Future<Nothing> provision;
  Future<Nothing> destroy;
  Promise<Option<vector<Path>>> promise;

  MockBackend* backend = new MockBackend();
  EXPECT_CALL(*backend, provision(_, _, _))
    .WillOnce(DoAll(FutureSatisfy(&provision),
                    Return(promise.future())));

  EXPECT_CALL(*backend, destroy(_, _))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(true)));

  hashmap<string, Owned<Backend>> backends =
    {{"MockBackend", Owned<Backend>(backend)}};

  Try<Owned<Provisioner>> provisioner = Provisioner::create(
      flags,
      provisionerDir,
      "MockBackend",
      backends);

  ASSERT_SOME(provisioner);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      provisioner->share(),
      vector<Owned<Isolator>>());

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Image image;
  image.set_type(Image::DOCKER);
  Image::Docker dockerImage;
  dockerImage.set_name("alpine");
  image.mutable_docker()->CopyFrom(dockerImage);

  ContainerInfo::MesosInfo mesosInfo;
  mesosInfo.mutable_image()->CopyFrom(image);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->CopyFrom(mesosInfo);

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");

  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());
  taskInfo.mutable_container()->CopyFrom(containerInfo);

  ExecutorInfo executorInfo = createExecutorInfo("executor", "exit 0");
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(taskInfo, executorInfo, sandbox.get()),
      map<string, string>(),
      None());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(provision);

  // Destroying container in `PROVISIONING` state will discard the container
  // launch immediately.
  containerizer->destroy(containerId);

  AWAIT_DISCARDED(launch);

  ASSERT_TRUE(destroy.isPending());
  ASSERT_TRUE(wait.isPending());

  promise.set(Option<vector<Path>>::none());

  AWAIT_READY(destroy);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  ContainerTermination termination = wait->get();

  EXPECT_FALSE(termination.has_status());
}


// This test verifies that isolator cleanup will not be invoked
// if the containerizer destroy a container while it is provisioning
// an image, because isolators are not prepared yet.
TEST_F(MesosContainerizerProvisionerTest, IsolatorCleanupBeforePrepare)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  Owned<Launcher> launcher(new TestLauncher(Owned<Launcher>(launcher_.get())));

  MockProvisioner* provisioner = new MockProvisioner();

  Future<Nothing> provision;
  Future<Nothing> destroy;
  Promise<ProvisionInfo> provisionPromise;
  Promise<bool> destroyPromise;

  EXPECT_CALL(*provisioner, provision(_, _))
    .WillOnce(DoAll(FutureSatisfy(&provision),
                    Return(provisionPromise.future())));

  EXPECT_CALL(*provisioner, destroy(_))
    .WillOnce(DoAll(FutureSatisfy(&destroy),
                    Return(destroyPromise.future())));

  MockIsolator* isolator = new MockIsolator();

  EXPECT_CALL(*isolator, cleanup(_))
    .Times(0);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      Shared<Provisioner>(provisioner),
      {Owned<Isolator>(isolator)});

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Image image;
  image.set_type(Image::DOCKER);
  Image::Docker dockerImage;
  dockerImage.set_name(id::UUID::random().toString());
  image.mutable_docker()->CopyFrom(dockerImage);

  ContainerInfo::MesosInfo mesosInfo;
  mesosInfo.mutable_image()->CopyFrom(image);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->CopyFrom(mesosInfo);

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");
  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());
  taskInfo.mutable_container()->CopyFrom(containerInfo);

  ExecutorInfo executorInfo = createExecutorInfo("executor", "exit 0");
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(taskInfo, executorInfo, sandbox.get()),
      map<string, string>(),
      None());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(provision);

  containerizer->destroy(containerId);

  AWAIT_READY(destroy);

  ASSERT_TRUE(wait.isPending());

  provisionPromise.set(ProvisionInfo{"rootfs", None()});

  AWAIT_DISCARDED(launch);

  destroyPromise.set(true);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  ContainerTermination termination = wait->get();

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
  // Create a TestLauncher backed by SubprocessLauncher.
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher_ = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher_);

  TestLauncher* testLauncher =
    new TestLauncher(Owned<Launcher>(launcher_.get()));

  Owned<Launcher> launcher(testLauncher);

  Fetcher fetcher(flags);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      provisioner->share(),
      vector<Owned<Isolator>>());

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  SlaveID slaveId = SlaveID();
  slaveId.set_value("slave_id");
  TaskInfo taskInfo = createTask(slaveId, Resources(), CommandInfo());

  // Destroy the container using the SubprocessLauncher but return a failed
  // future to the containerizer.
  EXPECT_CALL(*testLauncher, destroy(_))
    .WillOnce(DoAll(InvokeDestroyAndWait(testLauncher),
                    Return(Failure("Destroy failure"))));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          taskInfo,
          createExecutorInfo("executor", "sleep 1000"),
          sandbox.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  // The container destroy should fail.
  AWAIT_FAILED(termination);

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
  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ExecutorID executorId;
  executorId.set_value(id::UUID::random().toString());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

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
  frameworkId.set_value(id::UUID::random().toString());
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = containerizer->recover(slaveState);
  AWAIT_READY(recover);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_TRUE(containers->empty());
}


class MesosLauncherStatusTest : public MesosTest {};


// Check that we get the proper PID from launcher
// Using a invalid container ID should return a failure.
TEST_F(MesosLauncherStatusTest, ExecutorPIDTest)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "posix";

  Try<Launcher*> launcher = SubprocessLauncher::create(flags);
  ASSERT_SOME(launcher);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  ContainerID invalidContainerId;
  invalidContainerId.set_value(id::UUID::random().toString());

  Try<pid_t> forked = launcher.get()->fork(
      containerId,
      path::join(flags.launcher_dir, MESOS_CONTAINERIZER),
      vector<string>(),
      mesos::slave::ContainerIO(),
      nullptr,
      None(),
      None(),
      None(),
      vector<int_fd>());

  ASSERT_SOME(forked);

  Future<ContainerStatus> validStatus = launcher.get()->status(containerId);

  AWAIT_READY(validStatus);
  EXPECT_EQ(static_cast<pid_t>(validStatus->executor_pid()), forked.get());

  Future<ContainerStatus> invalidStatus =
    launcher.get()->status(invalidContainerId);

  AWAIT_FAILED(invalidStatus);

  AWAIT_READY(launcher.get()->destroy(containerId));
}


class MesosContainerizerWaitTest : public MesosTest {};


// Ensures the containerizer responds correctly (returns None)
// to a request to wait on an unknown container.
TEST_F(MesosContainerizerWaitTest, WaitUnknownContainer)
{
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  EXPECT_NONE(wait.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
