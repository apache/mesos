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

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::containerizer::paths::getRuntimePath;
using mesos::internal::slave::containerizer::paths::getSandboxPath;
using mesos::internal::slave::containerizer::paths::buildPath;
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
  : public ContainerizerTest<slave::MesosContainerizer>
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
    frameworkId.set_value(UUID::random().toString());
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
};


TEST_F(NestedMesosContainerizerTest, NestedContainerID)
{
  ContainerID id1;
  id1.set_value(UUID::random().toString());

  ContainerID id2;
  id2.set_value(UUID::random().toString());

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


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_LaunchNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("exit 42"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_LaunchNestedDebugCheckPidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

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
      createCommandInfo(
          "PS_LINES=`ps | wc -l`;"
          "if [ ${PS_LINES} -ne 6 ]; then"
          "  exit ${PS_LINES};"
          "fi;"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

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
      createCommandInfo(
          "PS_LINES=`ps | wc -l`;"
          "if [ ${PS_LINES} -le 6 ]; then"
          "  exit ${PS_LINES};"
          "fi;"),
      None(),
      None(),
      state.id,
      ContainerClass::DEBUG);

  AWAIT_ASSERT_TRUE(launch);

  wait = containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_CURL_INTERNET_LaunchNestedDebugCheckMntNamespace)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux,namespaces/pid";
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Fetcher fetcher;

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

  // Use a pipe to synchronize with the top-level container.
  Try<std::array<int, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  const std::array<int, 2>& pipes = pipes_.get();

  // Launch a command task within the `alpine` docker image and
  // synchronize its launch with the launch of a debug container below.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "echo running >&" + stringify(pipes[1]) + ";" + "sleep 1000");

  task.mutable_container()->CopyFrom(createContainerInfo("alpine"));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers->at(0).id(), {task});

  // We wait wait up to 120 seconds
  // to download the docker image.
  AWAIT_READY_FOR(statusRunning, Seconds(120));
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  close(pipes[1]);

  // Wait for the parent container to start running its task
  // before launching a debug container inside it.
  AWAIT_READY(process::io::poll(pipes[0], process::io::READ));
  close(pipes[0]);

  ASSERT_TRUE(statusRunning->has_slave_id());
  ASSERT_TRUE(statusRunning->has_container_status());
  ASSERT_TRUE(statusRunning->container_status().has_container_id());

  // Launch a nested debug container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(
      statusRunning->container_status().container_id());
  nestedContainerId.set_value(UUID::random().toString());

  // Launch a debug container inside the command task and check for
  // the existence of a file we know to be inside the `alpine` docker
  // image (but not on the host filesystem).
  Future<bool> launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo(
          "LINES=`ls -la /etc/alpine-release | wc -l`;"
          "if [ ${LINES} -ne 1 ]; then"
          "  exit 1;"
          "fi;"),
      None(),
      None(),
      statusRunning->slave_id(),
      ContainerClass::DEBUG);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  driver.stop();
  driver.join();
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_DestroyDebugContainerOnRecover)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch a debug container which should be destroyed on recovery.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id,
      ContainerClass::DEBUG);

  AWAIT_ASSERT_TRUE(launch);

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

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(2u, containers->size());

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_DestroyNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedWait.get()->status());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_DestroyParent)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  containerizer->destroy(containerId);

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


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_ParentExit)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<std::array<int, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  const std::array<int, 2>& pipes = pipes_.get();

  // NOTE: We use a non-shell command here to use 'bash -c' to execute
  // the 'read', which deals with the file descriptor, because of a bug
  // in ubuntu dash. Multi-digit file descriptor is not supported in
  // ubuntu dash, which executes the shell command.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/bash");
  command.add_arguments("bash");
  command.add_arguments("-c");
  command.add_arguments("read key <&" + stringify(pipes[0]));

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("executor");
  executor.mutable_command()->CopyFrom(command);
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  close(pipes[0]); // We're never going to read.

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  close(pipes[1]); // Force the 'read key' to exit!

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

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  // Use a pipe to synchronize with the top-level container.
  Try<std::array<int, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  const std::array<int, 2>& pipes = pipes_.get();

  // NOTE: We use a non-shell command here to use 'bash -c' to execute
  // the 'echo', which deals with the file descriptor, because of a bug
  // in ubuntu dash. Multi-digit file descriptor is not supported in
  // ubuntu dash, which executes the shell command.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/bash");
  command.add_arguments("bash");
  command.add_arguments("-c");
  command.add_arguments(
      "echo running >&" + stringify(pipes[1]) + ";" + "sleep 1000");

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("executor");
  executor.mutable_command()->CopyFrom(command);
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  close(pipes[1]);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Wait for the parent container to start running its executor
  // process before sending it a signal.
  AWAIT_READY(process::io::poll(pipes[0], process::io::READ));
  close(pipes[0]);

  ASSERT_EQ(0u, os::kill(status->executor_pid(), SIGTERM));

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


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverNested)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

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
  EXPECT_EQ(pid, status->executor_pid());

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(nestedPid, status->executor_pid());

  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

  containerizer->destroy(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, nestedWait.get()->status());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  const string cgroup = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(containerId));
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_RecoverNestedLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

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
  nestedContainerId.set_value(UUID::random().toString());

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
  EXPECT_EQ(pid, status->executor_pid());

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId));

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverLauncherOrphanAndSingleNestedLauncherOrphan)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  string cgroup = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId));

  wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

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

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

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
  nestedContainerId1.set_value(UUID::random().toString());

  string cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId1, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(UUID::random().toString());

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
  EXPECT_EQ(pid, status->executor_pid());

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

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverNestedContainersWithLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "sleep 1000",
      "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch the first nested container.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId1,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

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
  nestedContainerId2.set_value(UUID::random().toString());

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
  EXPECT_EQ(pid, status->executor_pid());

  status = containerizer->status(nestedContainerId1);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(nestedPid1, status->executor_pid());

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId2));

  wait = containerizer->wait(nestedContainerId1);

  containerizer->destroy(nestedContainerId1);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // We expect a wait status of SIGKILL on the nested container.
  // Since the kernel will destroy these via a SIGKILL, we expect
  // a SIGKILL here.
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RecoverLauncherOrphanAndMultipleNestedLauncherOrphans)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  string cgroup = path::join(
      flags.cgroups_root,
      buildPath(containerId, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId1, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      buildPath(nestedContainerId2, "mesos", JOIN));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup, true));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> nestedWait1 = containerizer->wait(
      nestedContainerId1);

  Future<Option<ContainerTermination>> nestedWait2 = containerizer->wait(
      nestedContainerId2);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(nestedWait1);
  ASSERT_SOME(nestedWait1.get());

  AWAIT_READY(nestedWait2);
  ASSERT_SOME(nestedWait2.get());

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_FALSE(containers->contains(nestedContainerId1));
  ASSERT_FALSE(containers->contains(nestedContainerId2));
  ASSERT_FALSE(containers->contains(containerId));
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_WaitAfterDestroy)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveID slaveId = SlaveID();

  // Launch a top-level container.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      slaveId,
      map<string, string>(),
      true);

  AWAIT_ASSERT_TRUE(launch);

  // Launch a nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("exit 42"),
      None(),
      None(),
      slaveId);

  AWAIT_ASSERT_TRUE(launch);

  // Wait once (which does a destroy),
  // then wait again on the nested container.
  Future<Option<ContainerTermination>> nestedWait = containerizer->wait(
      nestedContainerId);

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
  Future<Option<ContainerTermination>> wait = containerizer->wait(
      containerId);

  AWAIT_READY(containerizer->destroy(containerId));

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  // Wait on nested container again.
  nestedWait = containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_NONE(nestedWait.get());
}


// This test verifies that agent environment variables are not leaked
// to the nested container, and the environment variables specified in
// the command for the nested container will be honored.
TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_Environment)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

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
      command,
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_LaunchNestedThreeLevels)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID level1ContainerId;
  level1ContainerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      level1ContainerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  ContainerID level2ContainerId;
  level2ContainerId.mutable_parent()->CopyFrom(level1ContainerId);
  level2ContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      level2ContainerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  ContainerID level3ContainerId;
  level3ContainerId.mutable_parent()->CopyFrom(level2ContainerId);
  level3ContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      level3ContainerId,
      createCommandInfo("exit 42"),
      None(),
      None(),
      state.id);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(level3ContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());

  wait = containerizer->wait(level1ContainerId);

  containerizer->destroy(level1ContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(NestedMesosContainerizerTest, ROOT_CGROUPS_Remove)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("true"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // The runtime and sandbox directories must exist.
  const string runtimePath =
    getRuntimePath(flags.runtime_dir, nestedContainerId);
  ASSERT_TRUE(os::exists(runtimePath));

  const string sandboxPath = getSandboxPath(directory.get(), nestedContainerId);
  ASSERT_TRUE(os::exists(sandboxPath));

  // Now remove the nested container.
  Future<Nothing> remove = containerizer->remove(nestedContainerId);
  AWAIT_READY(remove);

  // We now expect the runtime and sandbox directories NOT to exist.
  EXPECT_FALSE(os::exists(runtimePath));
  EXPECT_FALSE(os::exists(sandboxPath));

  // We expect `remove` to be idempotent.
  remove = containerizer->remove(nestedContainerId);
  AWAIT_READY(remove);

  // Finally destroy the parent container.
  containerizer->destroy(containerId);

  wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
}


TEST_F(NestedMesosContainerizerTest,
       ROOT_CGROUPS_RemoveAfterParentDestroyed)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

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
  containerId.set_value(UUID::random().toString());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", "sleep 1000", "cpus:1"),
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to test not checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      createCommandInfo("true"),
      None(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

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
  containerizer->destroy(containerId);

  wait = containerizer->wait(containerId);
  AWAIT_READY(wait);

  // We expect `remove` to fail.
  Future<Nothing> remove = containerizer->remove(nestedContainerId);
  AWAIT_FAILED(remove);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
