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

#include <unistd.h>

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <mesos/module/isolator.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/abort.hpp>
#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif // __linux__

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/filesystem/shared.hpp"
#endif // __linux__
#include "slave/containerizer/mesos/isolators/posix.hpp"

#include "slave/containerizer/mesos/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"
#endif // __linux__

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;
#ifdef __linux__
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::LinuxLauncher;
using mesos::internal::slave::SharedFilesystemIsolatorProcess;
#endif // __linux__
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

using std::ostringstream;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class SharedFilesystemIsolatorTest : public MesosTest {};


// Test that a container can create a private view of a system
// directory (/var/tmp). Check that a file written by a process inside
// the container doesn't appear on the host filesystem but does appear
// under the container's work directory.
// This test is disabled since we're planning to remove the shared
// filesystem isolator and this test is not working on other distros
// such as CentOS 7.1
// TODO(tnachen): Remove this test when shared filesystem isolator
// is removed.
TEST_F(SharedFilesystemIsolatorTest, DISABLED_ROOT_RelativeVolume)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/shared";

  Try<Isolator*> _isolator = SharedFilesystemIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  // Use /var/tmp so we don't mask the work directory (under /tmp).
  const string containerPath = "/var/tmp";
  ASSERT_TRUE(os::stat::isdir(containerPath));

  // Use a host path relative to the container work directory.
  const string hostPath = strings::remove(containerPath, "/", strings::PREFIX);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->CopyFrom(ContainerInfo::MesosInfo());
  containerInfo.add_volumes()->CopyFrom(
      CREATE_VOLUME(containerPath, hostPath, Volume::RW));

  ExecutorInfo executorInfo;
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(flags.work_dir);

  Future<Option<ContainerLaunchInfo>> prepare =
    isolator->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(prepare);
  ASSERT_SOME(prepare.get());
  ASSERT_EQ(1, prepare.get().get().pre_exec_commands().size());
  EXPECT_TRUE(prepare.get().get().has_namespaces());

  // The test will touch a file in container path.
  const string file = path::join(containerPath, UUID::random().toString());
  ASSERT_FALSE(os::exists(file));

  // Manually run the isolator's preparation command first, then touch
  // the file.
  vector<string> args;
  args.push_back("sh");
  args.push_back("-x");
  args.push_back("-c");
  args.push_back(
      prepare.get().get().pre_exec_commands(0).value() + " && touch " + file);

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      args,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      prepare.get().get().namespaces());
  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Check the correct hierarchy was created under the container work
  // directory.
  string dir = "/";
  foreach (const string& subdir, strings::tokenize(containerPath, "/")) {
    dir = path::join(dir, subdir);

    struct stat hostStat;
    EXPECT_EQ(0, ::stat(dir.c_str(), &hostStat));

    struct stat containerStat;
    EXPECT_EQ(0,
              ::stat(path::join(flags.work_dir, dir).c_str(), &containerStat));

    EXPECT_EQ(hostStat.st_mode, containerStat.st_mode);
    EXPECT_EQ(hostStat.st_uid, containerStat.st_uid);
    EXPECT_EQ(hostStat.st_gid, containerStat.st_gid);
  }

  // Check it did *not* create a file in the host namespace.
  EXPECT_FALSE(os::exists(file));

  // Check it did create the file under the container's work directory
  // on the host.
  EXPECT_TRUE(os::exists(path::join(flags.work_dir, file)));
}


// This test is disabled since we're planning to remove the shared
// filesystem isolator and this test is not working on other distros
// such as CentOS 7.1
// TODO(tnachen): Remove this test when shared filesystem isolator
// is removed.
TEST_F(SharedFilesystemIsolatorTest, DISABLED_ROOT_AbsoluteVolume)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/shared";

  Try<Isolator*> _isolator = SharedFilesystemIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  // We'll mount the absolute test work directory as /var/tmp in the
  // container.
  const string hostPath = flags.work_dir;
  const string containerPath = "/var/tmp";

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->CopyFrom(ContainerInfo::MesosInfo());
  containerInfo.add_volumes()->CopyFrom(
      CREATE_VOLUME(containerPath, hostPath, Volume::RW));

  ExecutorInfo executorInfo;
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(flags.work_dir);

  Future<Option<ContainerLaunchInfo>> prepare =
    isolator->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(prepare);
  ASSERT_SOME(prepare.get());
  ASSERT_EQ(1, prepare.get().get().pre_exec_commands().size());
  EXPECT_TRUE(prepare.get().get().has_namespaces());

  // Test the volume mounting by touching a file in the container's
  // /tmp, which should then be in flags.work_dir.
  const string filename = UUID::random().toString();
  ASSERT_FALSE(os::exists(path::join(containerPath, filename)));

  vector<string> args;
  args.push_back("sh");
  args.push_back("-x");
  args.push_back("-c");
  args.push_back(prepare.get().get().pre_exec_commands(0).value() +
                 " && touch " +
                 path::join(containerPath, filename));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      args,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      prepare.get().get().namespaces());
  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Check the file was created in flags.work_dir.
  EXPECT_TRUE(os::exists(path::join(hostPath, filename)));

  // Check it didn't get created in the host's view of containerPath.
  EXPECT_FALSE(os::exists(path::join(containerPath, filename)));
}


class NamespacesPidIsolatorTest : public MesosTest {};


TEST_F(NamespacesPidIsolatorTest, ROOT_PidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,namespaces/pid";

  string directory = os::getcwd(); // We're inside a temporary sandbox.

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Write the command's pid namespace inode and init name to files.
  const string command =
    "stat -c %i /proc/self/ns/pid > ns && (cat /proc/1/comm > init)";

  process::Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      CREATE_EXECUTOR_INFO("executor", command),
      directory,
      None(),
      SlaveID(),
      std::map<string, string>(),
      false);
  AWAIT_READY(launch);
  ASSERT_TRUE(launch.get());

  // Wait on the container.
  Future<ContainerTermination> wait = containerizer->wait(containerId);
  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  // Check that the command was run in a different pid namespace.
  Try<ino_t> testPidNamespace = ns::getns(::getpid(), "pid");
  ASSERT_SOME(testPidNamespace);

  Try<string> containerPidNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerPidNamespace);

  EXPECT_NE(stringify(testPidNamespace.get()),
            strings::trim(containerPidNamespace.get()));

  // Check that 'sh' is the container's 'init' process.
  // This verifies that /proc has been correctly mounted for the container.
  Try<string> init = os::read(path::join(directory, "init"));
  ASSERT_SOME(init);

  EXPECT_EQ("sh", strings::trim(init.get()));
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
