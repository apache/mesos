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

#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/strings.hpp>

#include <mesos/mesos.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/flags.hpp"
#include "tests/isolator.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using std::map;
using std::string;
using std::vector;

class MesosContainerizerIsolatorPreparationTest :
  public tests::TemporaryDirectoryTest
{
public:
  // Construct a MesosContainerizer with TestIsolator(s) which use the provided
  // 'prepare' command(s).
  Try<MesosContainerizer*> CreateContainerizer(
      const vector<Option<CommandInfo> >& prepares)
  {
    vector<Owned<Isolator> > isolators;

    foreach (const Option<CommandInfo>& prepare, prepares) {
      Try<Isolator*> isolator = tests::TestIsolatorProcess::create(prepare);
      if (isolator.isError()) {
        return Error(isolator.error());
      }

      isolators.push_back(Owned<Isolator>(isolator.get()));
    }

    slave::Flags flags;
    flags.launcher_dir = path::join(tests::flags.build_dir, "src");

    Try<Launcher*> launcher = PosixLauncher::create(flags);
    if (launcher.isError()) {
      return Error(launcher.error());
    }

    return new MesosContainerizer(
        flags,
        false,
        Owned<Launcher>(launcher.get()),
        isolators);
  }


  Try<MesosContainerizer*> CreateContainerizer(
      const Option<CommandInfo>& prepare)
  {
    vector<Option<CommandInfo> > prepares;
    prepares.push_back(prepare);

    return CreateContainerizer(prepares);
  }
};


// The isolator has a prepare command that succeeds.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptSucceeds)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file = path::join(directory, "child.script.executed");

  Try<MesosContainerizer*> containerizer = CreateContainerizer(
      CREATE_COMMAND_INFO("touch " + file));
  CHECK_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  process::Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
      false);

  // Wait until the launch completes.
  AWAIT_READY(launch);

  // Wait for the child (preparation script + executor) to complete.
  process::Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);
  AWAIT_READY(wait);

  // Check the child exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  // Check the preparation script actually ran.
  EXPECT_TRUE(os::exists(file));

  // Destroy the container.
  containerizer.get()->destroy(containerId);

  delete containerizer.get();
}


// The isolator has a prepare command that fails.
TEST_F(MesosContainerizerIsolatorPreparationTest, ScriptFails)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file = path::join(directory, "child.script.executed");

  Try<MesosContainerizer*> containerizer = CreateContainerizer(
      CREATE_COMMAND_INFO("touch " + file + " && exit 1"));
  CHECK_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
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

  delete containerizer.get();
}


// There are two isolators, one with a prepare command that succeeds and
// another that fails. The execution order is not defined but the launch should
// fail from the failing prepare command.
TEST_F(MesosContainerizerIsolatorPreparationTest, MultipleScripts)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.
  string file1 = path::join(directory, "child.script.executed.1");
  string file2 = path::join(directory, "child.script.executed.2");

  vector<Option<CommandInfo> > prepares;
  // This isolator prepare command one will succeed if called first, otherwise
  // it won't get run.
  prepares.push_back(CREATE_COMMAND_INFO("touch " + file1 + " && exit 0"));
  // This will fail, either first or after the successful command.
  prepares.push_back(CREATE_COMMAND_INFO("touch " + file2 + " && exit 1"));

  Try<MesosContainerizer*> containerizer = CreateContainerizer(prepares);
  CHECK_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", "exit 0"),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
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

  delete containerizer.get();
}


class MesosContainerizerExecuteTest : public tests::TemporaryDirectoryTest {};

TEST_F(MesosContainerizerExecuteTest, IoRedirection)
{
  string directory = os::getcwd(); // We're inside a temporary sandbox.

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  // Use local=false so std{err,out} are redirected to files.
  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false);
  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value("test_container");

  string errMsg = "this is stderr";
  string outMsg = "this is stdout";
  string command =
    "(echo '" + errMsg + "' 1>&2) && echo '" + outMsg + "'";

  process::Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", command),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  process::Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);
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

  delete containerizer.get();
}
