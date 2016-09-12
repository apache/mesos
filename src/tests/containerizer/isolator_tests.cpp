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
#include "slave/containerizer/mesos/isolators/cgroups/perf_event.hpp"
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
using mesos::internal::slave::CgroupsPerfEventIsolatorProcess;
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
class PerfEventIsolatorTest : public MesosTest {};


TEST_F(PerfEventIsolatorTest, ROOT_CGROUPS_PERF_Sample)
{
  slave::Flags flags;

  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);

  Try<Isolator*> _isolator = CgroupsPerfEventIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  ExecutorInfo executorInfo;

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  AWAIT_READY(isolator->prepare(
      containerId,
      containerConfig));

  // This first sample is likely to be empty because perf hasn't
  // completed yet but we should still have the required fields.
  Future<ResourceStatistics> statistics1 = isolator->usage(containerId);
  AWAIT_READY(statistics1);
  ASSERT_TRUE(statistics1.get().has_perf());
  EXPECT_TRUE(statistics1.get().perf().has_timestamp());
  EXPECT_TRUE(statistics1.get().perf().has_duration());

  // Wait until we get the next sample. We use a generous timeout of
  // two seconds because we currently have a one second reap interval;
  // when running perf with perf_duration of 250ms we won't notice the
  // exit for up to one second.
  ResourceStatistics statistics2;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> statistics = isolator->usage(containerId);
    AWAIT_READY(statistics);

    statistics2 = statistics.get();

    ASSERT_TRUE(statistics2.has_perf());

    if (statistics1.get().perf().timestamp() !=
        statistics2.perf().timestamp()) {
      break;
    }

    os::sleep(Milliseconds(250));
    waited += Milliseconds(250);
  } while (waited < Seconds(2));

  sleep(2);

  EXPECT_NE(statistics1.get().perf().timestamp(),
            statistics2.perf().timestamp());

  EXPECT_TRUE(statistics2.perf().has_cycles());
  EXPECT_LE(0u, statistics2.perf().cycles());

  EXPECT_TRUE(statistics2.perf().has_task_clock());
  EXPECT_LE(0.0, statistics2.perf().task_clock());

  AWAIT_READY(isolator->cleanup(containerId));
}


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
  flags.isolation = "namespaces/pid";

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


// Username for the unprivileged user that will be created to test
// unprivileged cgroup creation. It will be removed after the tests.
// It is presumed this user does not normally exist.
const string UNPRIVILEGED_USERNAME = "mesos.test.unprivileged.user";


template <typename T>
class UserCgroupIsolatorTest
  : public ContainerizerTest<slave::MesosContainerizer>
{
public:
  static void SetUpTestCase()
  {
    ContainerizerTest<slave::MesosContainerizer>::SetUpTestCase();

    // Remove the user in case it wasn't cleaned up from a previous
    // test.
    os::system("userdel -r " + UNPRIVILEGED_USERNAME + " > /dev/null");

    ASSERT_EQ(0, os::system("useradd " + UNPRIVILEGED_USERNAME));
  }


  static void TearDownTestCase()
  {
    ContainerizerTest<slave::MesosContainerizer>::TearDownTestCase();

    ASSERT_EQ(0, os::system("userdel -r " + UNPRIVILEGED_USERNAME));
  }
};


// Test all isolators that use cgroups.
typedef ::testing::Types<CgroupsPerfEventIsolatorProcess> CgroupsIsolatorTypes;


TYPED_TEST_CASE(UserCgroupIsolatorTest, CgroupsIsolatorTypes);


TYPED_TEST(UserCgroupIsolatorTest, ROOT_CGROUPS_PERF_UserCgroup)
{
  slave::Flags flags = UserCgroupIsolatorTest<TypeParam>::CreateSlaveFlags();
  flags.perf_events = "cpu-cycles"; // Needed for CgroupsPerfEventIsolator.

  Try<Isolator*> _isolator = TypeParam::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("mem:1024;cpus:1").get()); // For cpu/mem isolators.

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(os::getcwd());
  containerConfig.set_user(UNPRIVILEGED_USERNAME);

  AWAIT_READY(isolator->prepare(
      containerId,
      containerConfig));

  // Isolators don't provide a way to determine the cgroups they use
  // so we'll inspect the cgroups for an isolated dummy process.
  pid_t pid = fork();
  if (pid == 0) {
    // Child just sleeps.
    ::sleep(100);

    ABORT("Child process should not reach here");
  }
  ASSERT_GT(pid, 0);

  AWAIT_READY(isolator->isolate(containerId, pid));

  // Get the container's cgroups from /proc/$PID/cgroup. We're only
  // interested in the cgroups that this isolator has created which we
  // can do explicitly by selecting those that have the path that
  // corresponds to the 'cgroups_root' slave flag. For example:
  //
  //   $ cat /proc/pid/cgroup
  //   6:blkio:/
  //   5:perf_event:/
  //   4:memory:/mesos/b7410ed8-c85b-445e-b50e-3a1698d0e18c
  //   3:freezer:/
  //   2:cpuacct:/
  //   1:cpu:/
  //
  // Our 'grep' will only select the 'memory' line and then 'awk' will
  // output 'memory/mesos/b7410ed8-c85b-445e-b50e-3a1698d0e18c'.
  Try<string> grepOut = os::shell(
      "grep '" + path::join("/", flags.cgroups_root) + "' /proc/" +
       stringify(pid) + "/cgroup | awk -F ':' '{print $2$3}'");

  ASSERT_SOME(grepOut);

  // Kill the dummy child process.
  ::kill(pid, SIGKILL);
  int exitStatus;
  EXPECT_NE(-1, ::waitpid(pid, &exitStatus, 0));

  vector<string> cgroups = strings::tokenize(grepOut.get(), "\n");
  ASSERT_FALSE(cgroups.empty());

  foreach (string cgroup, cgroups) {
    if (!os::exists(path::join(flags.cgroups_hierarchy, cgroup)) &&
        strings::startsWith(cgroup, "cpuacct,cpu")) {
      // An existing bug in CentOS 7.x causes 'cpuacct,cpu' cgroup to
      // be under 'cpu,cpuacct'. Actively detect this here to
      // work around this problem.
      vector<string> parts = strings::split(cgroup, "/");
      parts[0] = "cpu,cpuacct";
      cgroup = strings::join("/", parts);
    }

    // Check the user cannot manipulate the container's cgroup control
    // files.
    EXPECT_NE(0, os::system(
          "su - " + UNPRIVILEGED_USERNAME +
          " -c 'echo $$ >" +
          path::join(flags.cgroups_hierarchy, cgroup, "cgroup.procs") +
          "'"));

    // Check the user can create a cgroup under the container's
    // cgroup.
    string userCgroup = path::join(cgroup, "user");

    EXPECT_EQ(0, os::system(
          "su - " +
          UNPRIVILEGED_USERNAME +
          " -c 'mkdir " +
          path::join(flags.cgroups_hierarchy, userCgroup) +
          "'"));

    // Check the user can manipulate control files in the created
    // cgroup.
    EXPECT_EQ(0, os::system(
          "su - " +
          UNPRIVILEGED_USERNAME +
          " -c 'echo $$ >" +
          path::join(flags.cgroups_hierarchy, userCgroup, "cgroup.procs") +
          "'"));
  }

  // Clean up the container. This will also remove the nested cgroups.
  AWAIT_READY(isolator->cleanup(containerId));
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
