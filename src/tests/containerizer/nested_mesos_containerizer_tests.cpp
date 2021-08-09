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

#include <sys/stat.h>
#include <sys/wait.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/kill.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/composing.hpp"

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::containerizer::paths::getCgroupPath;
using mesos::internal::slave::containerizer::paths::getContainerConfig;
using mesos::internal::slave::containerizer::paths::getRuntimePath;
using mesos::internal::slave::containerizer::paths::getSandboxPath;
using mesos::internal::slave::containerizer::paths::buildPath;
using mesos::internal::slave::containerizer::paths::CONTAINER_CONFIG_FILE;
using mesos::internal::slave::containerizer::paths::JOIN;
using mesos::internal::slave::containerizer::paths::PREFIX;
using mesos::internal::slave::containerizer::paths::SUFFIX;

using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::RunState;
using mesos::internal::slave::state::SlaveState;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;

using std::map;
using std::ostringstream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class NestedMesosContainerizerTest
  : public ContainerizerTest<slave::MesosContainerizer>,
    public ::testing::WithParamInterface<bool>
{
protected:
  Try<SlaveState> createSlaveState(
      const ContainerID& containerId,
      const pid_t pid,
      const ExecutorInfo& executorInfo,
      const SlaveID& slaveId,
      const string& workDir)
  {
    // Construct a mock `SlaveState`.
    ExecutorState executorState;
    executorState.id = executorInfo.executor_id();
    executorState.info = executorInfo;
    executorState.latest = containerId;

    RunState runState;
    runState.id = containerId;
    runState.forkedPid = pid;
    executorState.runs.put(containerId, runState);

    FrameworkState frameworkState;
    frameworkState.executors.put(executorInfo.executor_id(), executorState);

    SlaveState slaveState;
    slaveState.id = slaveId;
    FrameworkID frameworkId;
    frameworkId.set_value(id::UUID::random().toString());
    slaveState.frameworks.put(frameworkId, frameworkState);

    // NOTE: The executor directory must exist for executor containers
    // otherwise when the containerizer recovers from the 'SlaveState'
    // it will fail.
    const string directory = slave::paths::getExecutorRunPath(
        workDir,
        slaveId,
        frameworkState.id,
        executorState.id,
        containerId);

    Try<Nothing> mkdir = os::mkdir(directory);
    if (mkdir.isError()) {
      return Error(
          "Failed to create directory '" + directory + "': " + mkdir.error());
    }

    return slaveState;
  }

  template <typename... Args>
  mesos::slave::ContainerConfig createNestedContainerConfig(
      const string& resources, Args... args) const
  {
    mesos::slave::ContainerConfig containerConfig =
      createContainerConfig(std::forward<Args>(args)...);

    const bool shareCgroups = GetParam();

    ContainerInfo* container = containerConfig.mutable_container_info();
    container->set_type(ContainerInfo::MESOS);
    container->mutable_linux_info()->set_share_cgroups(shareCgroups);

    if (!shareCgroups) {
      containerConfig.mutable_resources()->CopyFrom(
        Resources::parse(resources).get());
    }

    return containerConfig;
  }

  static bool awaitSynchronizationFile(const string& path)
  {
    Duration waited = Duration::zero();
    Duration interval = Milliseconds(1);

    do {
      if (os::exists(path)) {
        return true;
      }

      os::sleep(interval);
      waited += interval;
    } while (waited < process::TEST_AWAIT_TIMEOUT);

    return false;
  }
};


// Some nested containerizer tests are parameterized by the boolean
// `shared_cgroups` flag that specifies whether cgroups are shared
// between nested containers and their parent container.
INSTANTIATE_TEST_CASE_P(
    NestedContainerShareCgroups,
    NestedMesosContainerizerTest,
    ::testing::Values(true, false));


TEST_F(NestedMesosContainerizerTest, NestedContainerID)
{
  ContainerID id1;
  id1.set_value(id::UUID::random().toString());

  ContainerID id2;
  id2.set_value(id::UUID::random().toString());

  EXPECT_EQ(id1, id1);
  EXPECT_NE(id1, id2);

  ContainerID id3 = id1;
  id3.mutable_parent()->CopyFrom(id2);

  EXPECT_EQ(id3, id3);
  EXPECT_NE(id3, id1);

  hashset<ContainerID> ids;
  ids.insert(id2);
  ids.insert(id3);

  EXPECT_TRUE(ids.contains(id2));
  EXPECT_TRUE(ids.contains(id3));
  EXPECT_FALSE(ids.contains(id1));

  ostringstream out1;
  out1 << id1;
  EXPECT_EQ(id1.value(), out1.str());

  ostringstream out2;
  out2 << id3;
  EXPECT_EQ(strings::join(".", id2.value(), id3.value()), out2.str());
}


TEST_P(NestedMesosContainerizerTest, ROOT_CGROUPS_LaunchNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createNestedContainerConfig("cpus:0.1", createCommandInfo("exit 42")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that a separate cgroup is created for a nested
// container only if it does not share cgroups with its parent container.
TEST_P(NestedMesosContainerizerTest, ROOT_CGROUPS_LaunchNestedShareCgroups)
{
  const bool shareCgroups = GetParam();

  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createNestedContainerConfig("cpus:0.1", createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Check that a separate cgroup is created for a nested container only
  // if `share_cgroups` field is set to false.
  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  const string cgroup = getCgroupPath(flags.cgroups_root, nestedContainerId);

  ASSERT_NE(shareCgroups, cgroups::exists(cpuHierarchy.get(), cgroup));

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedTermination.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  // Check that the cgroups isolator cleaned up a nested cgroup
  // for the nested container.
  ASSERT_FALSE(cgroups::exists(cpuHierarchy.get(), cgroup));
}


// This test verifies that a debug container inherits the
// environment of its parent even after agent failover.
TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_DebugNestedContainerInheritsEnvironment)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  const string envKey = "MESOS_NESTED_INHERITS_ENVIRONMENT";
  const int32_t envValue = 42;
  mesos::Environment env = createEnvironment({{envKey, stringify(envValue)}});

  CommandInfo command = createCommandInfo("sleep 1000");
  command.mutable_environment()->CopyFrom(env);

  ExecutorInfo executor = createExecutorInfo("executor", command, "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Launch a nested debug container that accesses the
  // environment variable specified for its parent.
  {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            createCommandInfo("exit $" + envKey),
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(envValue, waitNested.get()->status());
  }

  // Launch a nested debug container that overwrites the
  // environment variable specified for its parent.
  {
    const int32_t envOverrideValue = 99;
    mesos::Environment env = createEnvironment(
        {{envKey, stringify(envOverrideValue)}});

    CommandInfo nestedCommand = createCommandInfo("exit $" + envKey);
    nestedCommand.mutable_environment()->CopyFrom(env);

    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            nestedCommand,
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(envOverrideValue, waitNested.get()->status());
  }

  // Force a delete on the containerizer to emulate recovery.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  // Launch a nested debug container that access the
  // environment variable specified for its parent.
  {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            createCommandInfo("exit $" + envKey),
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(envValue, waitNested.get()->status());
  }

  // Destroy the containerizer with all associated containers.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that a debug container
// shares MESOS_SANDBOX with its parent.
TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_DebugNestedContainerInheritsMesosSandbox)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string pipe = path::join(sandbox.get(), "pipe");
  ASSERT_EQ(0, ::mkfifo(pipe.c_str(), 0700));

  CommandInfo command =
    createCommandInfo("echo ${MESOS_SANDBOX} > " + pipe + " ; sleep 1000");

  ExecutorInfo executor = createExecutorInfo("executor", command, "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          executor,
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Launch a nested debug container that compares `MESOS_SANDBOX`
  // it sees with the one its parent sees.
  {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    CommandInfo nestedCommand = createCommandInfo(
        "read PARENT_SANDBOX < " + pipe + ";"
        "[ ${PARENT_SANDBOX} = ${MESOS_SANDBOX} ] && exit 0 || exit 1;");

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            nestedCommand,
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(0, waitNested.get()->status());
  }

  // Destroy the containerizer with all associated containers.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that a debug container shares the working
// directory with its parent even after agent failover.
TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_DebugNestedContainerInheritsWorkingDirectory)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  // Use a file to synchronize with the top-level container.
  string syncFile = path::join(sandbox.get(), "syncFile");

  const string filename = "nested_inherits_work_dir";

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "touch " + filename + "; touch " + syncFile + "; sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          executor,
          directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the parent container to start running its task
  // before launching a debug container inside it.
  ASSERT_TRUE(awaitSynchronizationFile(syncFile));

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Launch a nested debug container that access the file created by its parent.
  {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            createCommandInfo("ls " + filename),
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(0, waitNested.get()->status());
  }

  // Force a delete on the containerizer to emulate recovery.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  // Launch a nested debug container that access the file created by its parent.
  {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    Future<Containerizer::LaunchResult> launchNested = containerizer->launch(
        nestedContainerId,
        createContainerConfig(
            createCommandInfo("ls " + filename),
            None(),
            ContainerClass::DEBUG),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launchNested);

    Future<Option<ContainerTermination>> waitNested = containerizer->wait(
        nestedContainerId);

    AWAIT_READY(waitNested);
    ASSERT_SOME(waitNested.get());
    ASSERT_TRUE(waitNested.get()->has_status());
    EXPECT_WEXITSTATUS_EQ(0, waitNested.get()->status());
  }

  // Destroy the containerizer with all associated containers.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_LaunchNestedDebugCheckPidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  // Launch the nested container with the `ps | wc -l` command and
  // launch the container without a `ContainerClass`.  With this
  // default setting, we should request to clone a new PID namespace.
  //
  // We expect to see exactly 6 lines of output from `ps`.
  //
  // 1) The 'ps' header
  // 2) The init process of the container (i.e. `mesos-containerizer`).
  // 3) The executor of the container (i.e. `mesos-executor`).
  // 4) `sh`
  // 5) `wc -l`
  // 6) `ps`
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo(
          "PS_LINES=`ps | wc -l`;"
          "if [ ${PS_LINES} -ne 6 ]; then"
          "  exit ${PS_LINES};"
          "fi;")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Relaunch the nested container with the `ps | wc -l` command and
  // set the container class as `DEBUG`.  In this class, we don't
  // clone a new PID namespace.
  //
  // We expect to see much more than 6 lines of output from `ps`.
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo(
              "PS_LINES=`ps | wc -l`;"
              "if [ ${PS_LINES} -le 6 ]; then"
              "  exit ${PS_LINES};"
              "fi;"),
          None(),
          ContainerClass::DEBUG),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  wait = containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that nested container can share pid namespace
// with its parent container or have its own pid namespace based on
// the field `ContainerInfo.linux_info.share_pid_namespace`.
TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_LaunchNestedSharePidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  // Launch the parent container.
  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(),
          createExecutorInfo(
              "executor",
              "stat -Lc %i /proc/1/ns/pid > ns && sleep 1000",
              "cpus:1"),
          directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Launch the first nested container which will share pid namespace
  // with the parent container.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  ContainerInfo container;
  container.set_type(ContainerInfo::MESOS);
  container.mutable_linux_info()->set_share_pid_namespace(true);

  launch = containerizer->launch(
      nestedContainerId1,
      createContainerConfig(
          createCommandInfo("stat -Lc %i /proc/1/ns/pid > ns"),
          container),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId1);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Launch the second nested container which will have its own pid namespace.
  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  container.mutable_linux_info()->set_share_pid_namespace(false);

  launch = containerizer->launch(
      nestedContainerId2,
      createContainerConfig(
          createCommandInfo("stat -Lc %i /proc/1/ns/pid > ns"),
          container),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  wait = containerizer->wait(nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Get parent container's pid namespace.
  Try<string> parentPidNamespace = os::read(path::join(directory.get(), "ns"));
  ASSERT_SOME(parentPidNamespace);

  // Get first nested container's pid namespace.
  const string sandboxPath1 =
    getSandboxPath(directory.get(), nestedContainerId1);

  ASSERT_TRUE(os::exists(sandboxPath1));

  Try<string> pidNamespace1 = os::read(path::join(sandboxPath1, "ns"));
  ASSERT_SOME(pidNamespace1);

  // Get second nested container's pid namespace.
  const string sandboxPath2 =
    getSandboxPath(directory.get(), nestedContainerId2);

  ASSERT_TRUE(os::exists(sandboxPath2));

  Try<string> pidNamespace2 = os::read(path::join(sandboxPath2, "ns"));

  ASSERT_SOME(pidNamespace2);

  // Check the first nested container shares the pid namespace
  // with the parent container.
  EXPECT_EQ(stringify(parentPidNamespace.get()),
            stringify(pidNamespace1.get()));

  // Check the second nested container has its own pid namespace.
  EXPECT_NE(stringify(parentPidNamespace.get()),
            stringify(pidNamespace2.get()));

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_INTERNET_CURL_LaunchNestedDebugCheckMntNamespace)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux,namespaces/pid";
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  // Use a file to synchronize with the top-level container.
  string syncFile = path::join(sandbox.get(), "syncFile");

  // Launch a command task within the `alpine` docker image.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "touch /tmp/syncFile; sleep 1000");

  task.mutable_container()->CopyFrom(createContainerInfo(
      "alpine", {createVolumeHostPath("/tmp", sandbox.get(), Volume::RW)}));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->at(0).id(), {task});

  // We wait wait up to 120 seconds to download the docker image.
  AWAIT_READY_FOR(statusStarting, Seconds(120));
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(120));
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  // Wait for the parent container to start running its task
  // before launching a debug container inside it.
  ASSERT_TRUE(awaitSynchronizationFile(syncFile));

  ASSERT_TRUE(statusRunning->has_slave_id());
  ASSERT_TRUE(statusRunning->has_container_status());
  ASSERT_TRUE(statusRunning->container_status().has_container_id());

  // Launch a nested debug container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(
      statusRunning->container_status().container_id());
  nestedContainerId.set_value(id::UUID::random().toString());

  // Launch a debug container inside the command task and check for
  // the existence of a file we know to be inside the `alpine` docker
  // image (but not on the host filesystem).
  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo(
              "LINES=`ls -la /etc/alpine-release | wc -l`;"
              "if [ ${LINES} -ne 1 ]; then"
              "  exit 1;"
              "fi;"),
          None(),
          ContainerClass::DEBUG),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  driver.stop();
  driver.join();
}


// This test verifies detection of task's `mnt` namespace for a debug nested
// container. Debug nested container must enter `mnt` namespace of the task,
// so the agent tries to detect task's `mnt` namespace. This test launches
// a long-running task which runs a subtask that unshares `mnt` namespace.
// The structure of the resulting process tree is similar to the process tree
// of the command executor (the task of command executor unshares `mnt` ns):
//
// 0. root (aka "nanny"/"launcher" process) [root `mnt` namespace]
//   1. task: sleep 1000 [root `mnt` namespace]
//     2. subtaks: sleep 1000 [subtask's `mnt` namespace]
//
// We expect that the agent detects task's `mnt` namespace.
TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_LaunchNestedDebugAfterUnshareMntNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "filesystem/linux";

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

  // Launch the parent container.
  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string syncFile = path::join(sandbox.get(), "syncFile");

  const string cmd =
    "(unshare -m sh -c"
    " 'mkdir -p test_mnt; mount tmpfs -t tmpfs test_mnt;"
    " touch test_mnt/check; exec sleep 1000')&"
    "touch " + syncFile + "; exec sleep 1000";

  ExecutorInfo executor = createExecutorInfo("executor", cmd, "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for the parent container to start running its task
  // before launching a debug nested container.
  ASSERT_TRUE(awaitSynchronizationFile(syncFile));

  // Launch a nested debug container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  // Launch a debug container inside the command task and check for the
  // absence of a file we know to be inside the subtask's mounted directory.
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo(
              "LINES=`ls -la test_mnt/check | wc -l`;"
              "if [ ${LINES} -ne 0 ]; then"
              "  exit 1;"
              "fi;"),
          None(),
          ContainerClass::DEBUG),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_DestroyDebugContainerOnRecover)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch a debug container which should be destroyed on recovery.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo("sleep 1000"),
          None(),
          ContainerClass::DEBUG),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(2u, containers->size());

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_DestroyNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedTermination.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_DestroyParent)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->wait(nestedContainerId);

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedTermination.get()->status());

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_ParentExit)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  string pipe = path::join(sandbox.get(), "pipe");
  ASSERT_EQ(0, ::mkfifo(pipe.c_str(), 0700));

  // We launch a blocking `read` after which we return with a non-success code.
  ExecutorInfo executor = createExecutorInfo(
      "executor",
      createCommandInfo("read < " + pipe + " && exit 1"),
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  // Write to the fifo to unblock the `read` in the parent container.
  os::write(pipe, "");

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_NE(0, wait.get()->status());

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // We expect a wait status of SIGKILL on the nested container
  // because when the parent container is destroyed we expect any
  // nested containers to be destroyed as a result of destroying the
  // parent's pid namespace. Since the kernel will destroy these via a
  // SIGKILL, we expect a SIGKILL here.
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedWait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_ParentSigterm)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a file to synchronize with the top-level container.
  string syncFile = path::join(sandbox.get(), "syncFile");

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      createCommandInfo("touch " + syncFile + "; sleep 1000"),
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Wait for the parent container to start running its executor
  // process before sending it a signal.
  ASSERT_TRUE(awaitSynchronizationFile(syncFile));

  ASSERT_EQ(0, os::kill(status->executor_pid(), SIGTERM));

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGTERM, wait.get()->status());

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // We expect a wait status of SIGKILL on the nested container
  // because when the parent container is destroyed we expect any
  // nested containers to be destroyed as a result of destroying the
  // parent's pid namespace. Since the kernel will destroy these via a
  // SIGKILL, we expect a SIGKILL here.
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedWait.get()->status());
}


TEST_P(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createNestedContainerConfig("cpus:0.1", createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  const Result<mesos::slave::ContainerConfig> containerConfig =
    getContainerConfig(flags.runtime_dir, containerId);

  EXPECT_SOME(containerConfig);

  const Result<mesos::slave::ContainerConfig> nestedConfig =
    getContainerConfig(flags.runtime_dir, nestedContainerId);

  EXPECT_SOME(nestedConfig);

  pid_t nestedPid = status->executor_pid();

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, static_cast<pid_t>(status->executor_pid()));

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(nestedPid, static_cast<pid_t>(status->executor_pid()));

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedTermination.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that the agent could recover if the agent
// metadata is empty but container runtime dir is not cleaned
// up. This is a regression test for MESOS-8416.
TEST_P(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverNestedWithoutSlaveState)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createNestedContainerConfig("cpus:0.1", createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  // Pass an empty slave state to simulate that the agent metadata
  // is removed.
  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);
  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedWait.get()->status());

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_P(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverNestedWithoutConfig)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createNestedContainerConfig("cpus:0.1", createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  const Result<mesos::slave::ContainerConfig> containerConfig =
    getContainerConfig(flags.runtime_dir, containerId);

  EXPECT_SOME(containerConfig);

  const Result<mesos::slave::ContainerConfig> nestedConfig =
    getContainerConfig(flags.runtime_dir, nestedContainerId);

  EXPECT_SOME(nestedConfig);

  pid_t nestedPid = status->executor_pid();

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  // Remove the checkpointed nested container config to simulate the case
  // when we upgrade.
  string nestedConfigPath = path::join(
      getRuntimePath(flags.runtime_dir, containerId),
      CONTAINER_CONFIG_FILE);

  CHECK(os::exists(nestedConfigPath));
  CHECK_SOME(os::rm(nestedConfigPath));

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, static_cast<pid_t>(status->executor_pid()));

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(nestedPid, static_cast<pid_t>(status->executor_pid()));

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedTermination.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  // Now create a freezer cgroup that represents the container so
  // when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  const string cgroup = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroup));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  // We expect that containerizer recovery will detect orphan container and
  // will destroy it, so we check here that the freezer cgroup is destroyed.
  //
  // NOTE: `wait()` can return `Some` or `None` due to a race condition between
  // `recover()` and `______destroy()` for an orphan container.
  AWAIT_READY(containerizer->wait(containerId));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroup));

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(containerId));
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverNestedLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now create a freezer cgroup that represents the nested container
  // so when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  const string cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, static_cast<pid_t>(status->executor_pid()));

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId));

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverLauncherOrphanAndSingleNestedLauncherOrphan)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  // Now create a freezer cgroup that represents the container so
  // when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string cgroupParent = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroupParent, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroupParent));

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  string cgroupNested = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroupNested, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroupNested));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  // We expect that containerizer recovery will detect orphan containers and
  // will destroy them, so we check here that the freezer cgroups are destroyed.
  //
  // NOTE: `wait()` can return `Some` or `None` due to a race condition between
  // `recover()` and `______destroy()` for an orphan container.
  AWAIT_READY(containerizer->wait(nestedContainerId));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroupNested));

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId));

  AWAIT_READY(containerizer->wait(containerId));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroupParent));

  containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(containerId));
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverMultipleNestedLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now create a freezer cgroup that represents the nested container
  // so when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  string cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId1, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId2, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, static_cast<pid_t>(status->executor_pid()));

  Future<Option<ContainerTermination>> nestedWait1 = containerizer->wait(
      nestedContainerId1);

  Future<Option<ContainerTermination>> nestedWait2 = containerizer->wait(
      nestedContainerId2);

  AWAIT_READY(nestedWait1);
  ASSERT_SOME(nestedWait1.get());

  AWAIT_READY(nestedWait2);
  ASSERT_SOME(nestedWait2.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId1));
  ASSERT_FALSE(containers->contains(nestedContainerId2));

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverNestedContainersWithLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          state.id,
          executor.framework_id(),
          executor.executor_id(),
          containerId));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch the first nested container.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId1,
      createContainerConfig(createCommandInfo("sleep 1000")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  status = containerizer->status(nestedContainerId1);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t nestedPid1 = status->executor_pid();

  // Now create a freezer cgroup that represents the nested container
  // so when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  const string cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId2, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  // Force a delete on the containerizer before we create the new one.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState = createSlaveState(
      containerId,
      pid,
      executor,
      state.id,
      flags.work_dir);

  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, static_cast<pid_t>(status->executor_pid()));

  status = containerizer->status(nestedContainerId1);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(nestedPid1, static_cast<pid_t>(status->executor_pid()));

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId2));

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(nestedContainerId1);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  termination = containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverLauncherOrphanAndMultipleNestedLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  // Now create a freezer cgroup that represents the container so
  // when the LinuxLauncher recovers we'll treat it as an orphan.
  //
  // NOTE: `cgroups::hierarchy` must be called AFTER
  // `MesosContainerizer::create` which calls `LinuxLauncher::create`
  // which calls `cgroups::prepare`, otherwise we might not have a
  // 'freezer' hierarchy prepared yet!
  Result<string> freezerHierarchy = cgroups::hierarchy("freezer");
  ASSERT_SOME(freezerHierarchy);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string cgroupParent = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroupParent, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroupParent));

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  string cgroupNested1 = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId1, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroupNested1, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroupNested1));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  string cgroupNested2 = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId2, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroupNested2, true));
  ASSERT_TRUE(cgroups::exists(freezerHierarchy.get(), cgroupNested2));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  // We expect that containerizer recovery will detect orphan containers and
  // will destroy them, so we check here that the freezer cgroups are destroyed.
  //
  // NOTE: `wait()` can return `Some` or `None` due to a race condition between
  // `recover()` and `______destroy()` for an orphan container.
  AWAIT_READY(containerizer->wait(nestedContainerId1));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroupNested1));

  AWAIT_READY(containerizer->wait(nestedContainerId2));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroupNested2));

  AWAIT_READY(containerizer->wait(containerId));
  ASSERT_FALSE(cgroups::exists(freezerHierarchy.get(), cgroupParent));

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId1));
  ASSERT_FALSE(containers->contains(nestedContainerId2));
  ASSERT_FALSE(containers->contains(containerId));
}


// This test verifies that termination status of a nested container is
// available until its parent is terminated.
TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_WaitAfterDestroy)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveID slaveId = SlaveID();

  // Launch a top-level container.
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

  // Launch a nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("exit 42")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait once for a nested container completion, then wait again
  // to make sure that its termination status is still available.
  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  nestedWait = containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  // Destroy the top-level container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  // Wait on nested container again.
  nestedWait = containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_NONE(nestedWait.get());
}


// This test verifies that a container termination status for a terminated
// nested container is available via `wait()` and `destroy()` methods for
// both mesos and composing containerizers.
TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_TerminatedNestedStatus)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  MesosContainerizer* mesosContainerizer(create.get());

  Try<slave::ComposingContainerizer*> composing =
    slave::ComposingContainerizer::create({mesosContainerizer});

  ASSERT_SOME(composing);

  Owned<Containerizer> containerizer(composing.get());

  SlaveID slaveId = SlaveID();

  // Launch a top-level container.
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

  // Launch a nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("exit 42")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Verify that both `wait` and `destroy` methods of composing containerizer
  // return the same container termination for a terminated nested container.
  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  Future<Option<ContainerTermination>> nestedTermination =
    containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedTermination.get()->status());

  // Verify that both `wait` and `destroy` methods of mesos containerizer
  // return the same container termination for a terminated nested container.
  nestedWait = mesosContainerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  nestedTermination = mesosContainerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedTermination);
  ASSERT_SOME(nestedTermination.get());
  ASSERT_TRUE(nestedTermination.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedTermination.get()->status());

  // Destroy the top-level container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that agent environment variables are not leaked
// to the nested container, and the environment variables specified in
// the command for the nested container will be honored.
TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_AgentEnvironmentNotLeaked)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  // Construct a command that verifies that agent environment
  // variables are not leaked to the nested container.
  ostringstream script;
  script << "#!/bin/sh\n";

  foreachkey (const string& key, os::environment()) {
    script << "test -z \"$" << key << "\"\n";
  }

  mesos::Environment environment = createEnvironment(
      {{"NESTED_MESOS_CONTAINERIZER_TEST", "ENVIRONMENT"}});

  script << "test $NESTED_MESOS_CONTAINERIZER_TEST = ENVIRONMENT\n";

  CommandInfo command = createCommandInfo(script.str());
  command.mutable_environment()->CopyFrom(environment);

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(command),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_Remove)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("true")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // The runtime, config and sandbox directories must exist.
  const string runtimePath =
    getRuntimePath(flags.runtime_dir, nestedContainerId);
  ASSERT_TRUE(os::exists(runtimePath));

  const string sandboxPath = getSandboxPath(directory.get(), nestedContainerId);
  ASSERT_TRUE(os::exists(sandboxPath));

  const Result<mesos::slave::ContainerConfig> containerConfig =
    getContainerConfig(flags.runtime_dir, nestedContainerId);

  ASSERT_SOME(containerConfig);

  // Now remove the nested container.
  Future<Nothing> remove = containerizer->remove(nestedContainerId);
  AWAIT_READY(remove);

  // We now expect the runtime, config and sandbox directories NOT to exist.
  EXPECT_FALSE(os::exists(runtimePath));
  EXPECT_FALSE(os::exists(sandboxPath));

  // We expect `remove` to be idempotent.
  remove = containerizer->remove(nestedContainerId);
  AWAIT_READY(remove);

  // Finally destroy the parent container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RemoveAfterParentDestroyed)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
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

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo("true")),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // The runtime and sandbox directories of the nested container must exist.
  const string runtimePath =
    getRuntimePath(flags.runtime_dir, nestedContainerId);
  ASSERT_TRUE(os::exists(runtimePath));

  const string sandboxPath = getSandboxPath(directory.get(), nestedContainerId);
  ASSERT_TRUE(os::exists(sandboxPath));

  // Now destroy the parent container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);

  // We expect `remove` to fail.
  Future<Nothing> remove = containerizer->remove(nestedContainerId);
  AWAIT_FAILED(remove);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
