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

#include <sstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/kill.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using namespace process;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::RunState;
using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;

using std::ostringstream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


// class LinuxLauncherTest : public MesosTest
// {
// protected:
//   struct Parameters
//   {
//     vector<string> argv;
//     string path;
//     Subprocess::IO stdin = Subprocess::FD(STDIN_FILENO);
//     Subprocess::IO stdout = Subprocess::FD(STDOUT_FILENO);
//     Subprocess::IO stderr = Subprocess::FD(STDERR_FILENO);
//     slave::MesosContainerizerLaunch::Flags flags;
//   };

//   Try<Parameters> prepare(
//       const slave::Flags& flags,
//       const string& command,
//       const Option<string>& exitStatusCheckpointPath = None())
//   {
//     Parameters parameters;

//     parameters.path = path::join(flags.launcher_dir, "mesos-containerizer");

//     // TODO(benh): Reset `parameters.stdin`, `parameters.stdout`,
//     // `parameters.stderr` if `flags.verbose` is false.

//     parameters.argv.resize(2);
//     parameters.argv[0] = "mesos-containerizer";
//     parameters.argv[1] = slave::MesosContainerizerLaunch::NAME;

//     CommandInfo commandInfo;
//     commandInfo.set_shell(true);
//     commandInfo.set_value(command);

//     parameters.flags.command = JSON::protobuf(commandInfo);

//     Try<string> directory = environment->mkdtemp();
//     if (directory.isError()) {
//       return Error("Failed to create directory: " + directory.error());
//     }

//     parameters.flags.working_directory = directory.get();

//     if (exitStatusCheckpointPath.isSome()) {
//       parameters.flags.exit_status_path = exitStatusCheckpointPath.get();
//     }

//     return parameters;
//   }
// };


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_ForkNoCheckpointExitStatus)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(flags, "exit 0");
//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Fork)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "exit 0",
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Recover)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "exit 0",
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   delete launcher;

//   create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   launcher = create.get();

//   AWAIT_READY(launcher->recover({}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   ASSERT_TRUE(status->has_executor_pid());
//   EXPECT_EQ(pid.get(), status->executor_pid());

//   // Since process is just exiting we can `wait` before we `destroy`.
//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


//   // uncheckpointed container (i.e., an unknown launched container)

//   // orphan cgroup (i.e., a legacy partially launched container)


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_RecoverUncheckpointed)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   // Launch a process in a cgroup that the LinuxLauncher will assume
//   // is an uncheckpointed container.
//   //
//   // NOTE: we need to call `cgroups::hierarchy` this AFTER
//   // `LinuxLauncher::create` so that a `cgroups::prepare` will have
//   // been called already.
//   Result<string> hierarchy = cgroups::hierarchy("freezer");
//   ASSERT_SOME(hierarchy);

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   string cgroup = path::join(
//       flags.cgroups_root,
//       slave::Launcher::buildPath(containerId));

//   Try<string> directory = environment->mkdtemp();
//   ASSERT_SOME(directory);

//   Try<Subprocess> child = subprocess(
//       "exit 0",
//       Subprocess::FD(STDIN_FILENO),
//       Subprocess::FD(STDOUT_FILENO),
//       Subprocess::FD(STDERR_FILENO),
//       SETSID,
//       None(),
//       None(),
//       {Subprocess::Hook([=](pid_t pid) {
//         return cgroups::isolate(hierarchy.get(), cgroup, pid);
//       })},
//       directory.get());

//   ASSERT_SOME(child);

//   ContainerState state;
//   state.mutable_executor_info()->mutable_executor_id()->set_value("executor"); // NOLINT
//   state.mutable_container_id()->CopyFrom(containerId);
//   state.set_pid(child->pid());
//   state.set_directory(directory.get());

//   // Now make sure we can recover.
//   AWAIT_ASSERT_EQ(hashset<ContainerID>::EMPTY, launcher->recover({state}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   ASSERT_TRUE(status->has_executor_pid());
//   EXPECT_EQ(child->pid(), status->executor_pid());

//   // And now make sure we can wait on it, but not since this is an
//   // uncheckpointed container not launched by the launcher the exit
//   // status will be `None`.
//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_READY(wait);
//   EXPECT_NONE(wait.get());
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "exit 42",
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Check UTS namespace.
//   Try<ino_t> inode = ns::getns(pid.get(), "uts");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

//   // Check NET namespace.
//   inode = ns::getns(pid.get(), "net");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

//   // Check PID namespace.
//   inode = ns::getns(pid.get(), "pid");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

//   AWAIT_EXPECT_WEXITSTATUS_EQ(42, launcher->wait(nestedContainerId));
//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_READY(launcher->destroy(containerId));
//   AWAIT_EXPECT_WEXITSTATUS_NE(0, launcher->wait(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkDestroyNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Now destroy outer container.
//   Future<Option<int>> wait = launcher->wait(containerId);
//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, wait);
//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   AWAIT_READY(launcher->destroy(nestedContainerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkNestedDestroyNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Now destroy nested container.
//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, wait);
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedRecoverNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Check UTS namespace.
//   Try<ino_t> inode = ns::getns(pid.get(), "uts");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

//   // Check NET namespace.
//   inode = ns::getns(pid.get(), "net");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

//   // Check PID namespace.
//   inode = ns::getns(pid.get(), "pid");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

//   delete launcher;

//   create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   launcher = create.get();

//   AWAIT_READY(launcher->recover({}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   ASSERT_TRUE(status->has_executor_pid());
//   EXPECT_EQ(pid.get(), status->executor_pid());

//   status = launcher->status(nestedContainerId);
//   AWAIT_READY(status);
//   ASSERT_TRUE(status->has_executor_pid());
//   EXPECT_EQ(nestedPid.get(), status->executor_pid());

//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, wait);
// }


class MesosContainerizerTest
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


TEST_F(MesosContainerizerTest, NestedContainerID)
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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_Launch)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "exit 42");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, wait.get()->status());
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_Destroy)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_LaunchNested)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("exit 42"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_DestroyNested)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_LaunchNestedDestroyParent)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_LaunchNestedParentExit)
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

  int pipes[2] = {-1, -1};
  ASSERT_SOME(os::pipe(pipes));

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "executor",
      "read key <&" + stringify(pipes[0]));

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  close(pipes[0]); // We're never going to read.

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_LaunchNestedParentSigterm)
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
  int pipes[2] = {-1, -1};
  ASSERT_SOME(os::pipe(pipes));

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  close(pipes[1]);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_Recover)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);

  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState =
    createSlaveState(containerId, pid, executor, state.id, flags.work_dir);
  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, status->executor_pid());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_RecoverNested)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);

  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);

  ASSERT_TRUE(status->has_executor_pid());

  pid_t nestedPid = status->executor_pid();

  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState =
    createSlaveState(containerId, pid, executor, state.id, flags.work_dir);
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

  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

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


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_RecoverLauncherOrphans)
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
      slave::containerizer::paths::buildPath(containerId));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_FAILED(status);
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_RecoverNestedLauncherOrphans)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

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
      slave::containerizer::paths::buildPath(nestedContainerId));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState =
    createSlaveState(containerId, pid, executor, state.id, flags.work_dir);
  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, status->executor_pid());

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  status = containerizer->status(nestedContainerId);
  AWAIT_FAILED(status);

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(MesosContainerizerTest,
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
      slave::containerizer::paths::buildPath(containerId));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      slave::containerizer::paths::buildPath(nestedContainerId));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<ContainerStatus> status = containerizer->status(nestedContainerId);
  AWAIT_FAILED(status);

  wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  status = containerizer->status(containerId);
  AWAIT_FAILED(status);
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_RecoverMultiNestedLauncherOrphans)
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

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
      slave::containerizer::paths::buildPath(nestedContainerId1));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      slave::containerizer::paths::buildPath(nestedContainerId2));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState =
    createSlaveState(containerId, pid, executor, state.id, flags.work_dir);
  ASSERT_SOME(slaveState);

  state = slaveState.get();
  AWAIT_READY(containerizer->recover(state));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, status->executor_pid());

  Future<Option<ContainerTermination>> nestedWait1 =
    containerizer->wait(nestedContainerId1);

  Future<Option<ContainerTermination>> nestedWait2 =
    containerizer->wait(nestedContainerId2);

  AWAIT_READY(nestedWait1);
  ASSERT_SOME(nestedWait1.get());

  AWAIT_READY(nestedWait2);
  ASSERT_SOME(nestedWait2.get());

  status = containerizer->status(nestedContainerId1);
  AWAIT_FAILED(status);

  status = containerizer->status(nestedContainerId2);
  AWAIT_FAILED(status);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


TEST_F(MesosContainerizerTest,
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

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

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
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);

  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Now launch the first nested container.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(UUID::random().toString());

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId1,
      CREATE_COMMAND_INFO("sleep 1000"),
      None(),
      directory.get(),
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
      slave::containerizer::paths::buildPath(nestedContainerId2));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  Try<SlaveState> slaveState =
    createSlaveState(containerId, pid, executor, state.id, flags.work_dir);
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

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  status = containerizer->status(nestedContainerId2);
  AWAIT_FAILED(status);

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


TEST_F(MesosContainerizerTest,
       ROOT_CGROUPS_RecoverLauncherOrphanAndMultiNestedLauncherOrphans)
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
      slave::containerizer::paths::buildPath(containerId));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      slave::containerizer::paths::buildPath(nestedContainerId1));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(UUID::random().toString());

  cgroup = path::join(
      flags.cgroups_root,
      slave::containerizer::paths::buildPath(nestedContainerId2));

  ASSERT_SOME(cgroups::create(freezerHierarchy.get(), cgroup));

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Future<Option<ContainerTermination>> nestedWait1 =
    containerizer->wait(nestedContainerId1);

  Future<Option<ContainerTermination>> nestedWait2 =
    containerizer->wait(nestedContainerId2);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(nestedWait1);
  ASSERT_SOME(nestedWait1.get());

  AWAIT_READY(nestedWait2);
  ASSERT_SOME(nestedWait2.get());

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  Future<ContainerStatus> status = containerizer->status(nestedContainerId1);
  AWAIT_FAILED(status);

  status = containerizer->status(nestedContainerId2);
  AWAIT_FAILED(status);

  status = containerizer->status(containerId);
  AWAIT_FAILED(status);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
