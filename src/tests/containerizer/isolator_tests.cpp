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
#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/mem.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/net_cls.hpp"
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

#include "tests/containerizer/memory_test_helper.hpp"

using namespace process;

using mesos::internal::master::Master;
#ifdef __linux__
using mesos::internal::slave::CgroupsCpushareIsolatorProcess;
using mesos::internal::slave::CgroupsMemIsolatorProcess;
using mesos::internal::slave::CgroupsNetClsIsolatorProcess;
using mesos::internal::slave::CgroupsPerfEventIsolatorProcess;
using mesos::internal::slave::CPU_SHARES_PER_CPU_REVOCABLE;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::LinuxLauncher;
using mesos::internal::slave::NetClsHandle;
using mesos::internal::slave::NetClsHandleManager;
using mesos::internal::slave::SharedFilesystemIsolatorProcess;
#endif // __linux__
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::PosixCpuIsolatorProcess;
using mesos::internal::slave::PosixMemIsolatorProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

using process::http::OK;
using process::http::Response;

using std::ostringstream;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// Flag describing whether a the isolatePid parentHook should check the cgroups
// before and after isolation.
enum CheckCgroups
{
  CHECK_CGROUPS,
  NO_CHECK_CGROUPS,
};

// A hook that is executed in the parent process. It attempts to isolate
// a process and can optionally check the cgroups before and after isolation.
//
// NOTE: The child process is blocked by the hook infrastructure while
// these hooks are executed.
// NOTE: Returning an Error implies the child process will be killed.
Try<Nothing> isolatePid(
    pid_t child,
    const Owned<Isolator>& isolator,
    const ContainerID& containerId,
    const CheckCgroups checkCgroups = NO_CHECK_CGROUPS)
{
  if (checkCgroups == CHECK_CGROUPS) {
    // Before isolation, the cgroup is empty.
    Future<ResourceStatistics> usage = isolator->usage(containerId);

    // Note this is following the implementation of AWAIT_READY.
    if (!process::internal::await(usage, Seconds(15))) {
      return Error("Could check cgroup usage");
    }
    if (usage.isDiscarded() || usage.isFailed()) {
      return Error("Could check cgroup usage");
    }

    if (0U != usage.get().processes()) {
      return Error("Cgroups processes not empty before isolation");
    }
    if (0U != usage.get().threads()) {
      return Error("Cgroups threads not empty before isolation");
    }
  }

  // Isolate process.
  Future<Nothing> isolate = isolator->isolate(containerId, child);

  // Note this is following the implementation of AWAIT_READY.
  if (!process::internal::await(isolate, Seconds(15))) {
    return Error("Could not isolate pid");
  }
  if (isolate.isDiscarded() || isolate.isFailed()) {
    return Error("Could not isolate pid");
  }

  if (checkCgroups == CHECK_CGROUPS) {
    // After isolation, there should be one process in the cgroup.
    Future<ResourceStatistics> usage = isolator->usage(containerId);

    // Note this is following the implementation of AWAIT_READY.
    if (!process::internal::await(usage, Seconds(15))) {
      return Error("Could check cgroup usage");
    }
    if (usage.isDiscarded() || usage.isFailed()) {
      return Error("Could check cgroup usage");
    }

    if (1U != usage.get().processes()) {
      return Error("Cgroups processes empty after isolation");
    }
    if (1U != usage.get().threads()) {
      return Error("Cgroups threads empty after isolation");
    }
  }

  return Nothing();
}


template <typename T>
class CpuIsolatorTest : public MesosTest {};


typedef ::testing::Types<
    PosixCpuIsolatorProcess,
#ifdef __linux__
    CgroupsCpushareIsolatorProcess,
#endif // __linux__
    tests::Module<Isolator, TestCpuIsolator>> CpuIsolatorTypes;


TYPED_TEST_CASE(CpuIsolatorTest, CpuIsolatorTypes);


TYPED_TEST(CpuIsolatorTest, UserCpuUsage)
{
  slave::Flags flags;

  Try<Isolator*> _isolator = TypeParam::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  // A PosixLauncher is sufficient even when testing a cgroups isolator.
  Try<Launcher*> _launcher = PosixLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1.0").get());

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

  const string& file = path::join(dir.get(), "mesos_isolator_test_ready");

  // Max out a single core in userspace. This will run for at most onesecond.
  string command = "while true ; do true ; done &"
    "touch " + file + "; " // Signals the command is running.
    "sleep 60";

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  vector<Subprocess::Hook> parentHooks;

  // Create parent Hook to isolate child.
  //
  // NOTE: We can safely use references here as the hook will be executed
  // during the call to `fork`.
  const lambda::function<Try<Nothing>(pid_t)> isolatePidHook = lambda::bind(
      isolatePid,
      lambda::_1,
      std::cref(isolator),
      std::cref(containerId),
      NO_CHECK_CGROUPS);

  parentHooks.emplace_back(Subprocess::Hook(isolatePidHook));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      None(),
      parentHooks);

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Wait for the command to start.
  while (!os::exists(file));

  // Wait up to 1 second for the child process to induce 1/8 of a second of
  // user cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = isolator->usage(containerId);
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.cpus_user_time_secs() >= 0.125) {
      break;
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(1));

  EXPECT_LE(0.125, statistics.cpus_user_time_secs());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Make sure the child was reaped.
  AWAIT_READY(status);

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}


TYPED_TEST(CpuIsolatorTest, SystemCpuUsage)
{
  slave::Flags flags;

  Try<Isolator*> _isolator = TypeParam::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  // A PosixLauncher is sufficient even when testing a cgroups isolator.
  Try<Launcher*> _launcher = PosixLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1.0").get());

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

  const string& file = path::join(dir.get(), "mesos_isolator_test_ready");

  // Generating random numbers is done by the kernel and will max out a single
  // core and run almost exclusively in the kernel, i.e., system time.
  string command = "cat /dev/urandom > /dev/null & "
    "touch " + file + "; " // Signals the command is running.
    "sleep 60";

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  vector<Subprocess::Hook> parentHooks;

  // Create parent Hook to isolate child.
  //
  // NOTE: We can safely use references here as the hook will be executed
  // during the call to `fork`.
  const lambda::function<Try<Nothing>(pid_t)> isolatePidHook = lambda::bind(
      isolatePid,
      lambda::_1,
      std::cref(isolator),
      std::cref(containerId),
      NO_CHECK_CGROUPS);

  parentHooks.emplace_back(Subprocess::Hook(isolatePidHook));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      None(),
      parentHooks);

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Wait for the command to start.
  while (!os::exists(file));

  // Wait up to 1 second for the child process to induce 1/8 of a second of
  // system cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = isolator->usage(containerId);
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.cpus_system_time_secs() >= 0.125) {
      break;
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(1));

  EXPECT_LE(0.125, statistics.cpus_system_time_secs());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Make sure the child was reaped.
  AWAIT_READY(status);

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}


#ifdef __linux__
class NetClsHandleManagerTest : public testing::Test {};


// Tests the ability of the `NetClsHandleManager` class to allocate
// and free secondary handles from a range of primary handles.
TEST_F(NetClsHandleManagerTest, AllocateFreeHandles)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  Try<NetClsHandle> handle = manager.alloc(0x0003);
  ASSERT_SOME(handle);

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  ASSERT_SOME(manager.free(handle.get()));

  EXPECT_SOME_FALSE(manager.isUsed(handle.get()));
}


// Make sure allocation of secondary handles for invalid primary
// handles results in an error.
TEST_F(NetClsHandleManagerTest, AllocateInvalidPrimary)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  ASSERT_ERROR(manager.alloc(0x0001));
}


// Tests that we can reserve secondary handles for a given primary
// handle so that they won't be allocated out later.
TEST_F(NetClsHandleManagerTest, ReserveHandles)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  NetClsHandle handle(0x0003, 0xffff);

  ASSERT_SOME(manager.reserve(handle));

  EXPECT_SOME_TRUE(manager.isUsed(handle));
}


// Tests that secondary handles are allocated only from a given range,
// when the range is specified.
TEST_F(NetClsHandleManagerTest, SecondaryHandleRange)
{
  NetClsHandleManager manager(
      IntervalSet<uint32_t>(
        (Bound<uint32_t>::closed(0x0002),
         Bound<uint32_t>::closed(0x0003))),
      IntervalSet<uint32_t>(
        (Bound<uint32_t>::closed(0xffff),
         Bound<uint32_t>::closed(0xffff))));

  Try<NetClsHandle> handle = manager.alloc(0x0003);
  ASSERT_SOME(handle);

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  // Try allocating another handle. This should fail, since we don't
  // have any more secondary handles left.
  EXPECT_ERROR(manager.alloc(0x0003));

  ASSERT_SOME(manager.free(handle.get()));

  ASSERT_SOME(manager.reserve(handle.get()));

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  // Make sure you cannot reserve a secondary handle that is out of
  // range.
  EXPECT_ERROR(manager.reserve(NetClsHandle(0x0003, 0x0001)));
}
#endif // __linux__


#ifdef __linux__
class RevocableCpuIsolatorTest : public MesosTest {};


TEST_F(RevocableCpuIsolatorTest, ROOT_CGROUPS_RevocableCpu)
{
  slave::Flags flags;

  Try<Isolator*> _isolator = CgroupsCpushareIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = PosixLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  // Include revocable CPU in the executor's resources.
  Resource cpu = Resources::parse("cpus", "1", "*").get();
  cpu.mutable_revocable();

  ExecutorInfo executorInfo;
  executorInfo.add_resources()->CopyFrom(cpu);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(os::getcwd());

  AWAIT_READY(isolator->prepare(
        containerId,
        containerConfig));

  vector<string> argv{"sleep", "100"};

  Try<pid_t> pid = launcher->fork(
      containerId,
      "/bin/sleep",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      nullptr,
      None(),
      None());

  ASSERT_SOME(pid);

  AWAIT_READY(isolator->isolate(containerId, pid.get()));

  // Executor should have proper cpu.shares for revocable containers.
  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());
  ASSERT_SOME(cpuCgroup);

  EXPECT_SOME_EQ(
      CPU_SHARES_PER_CPU_REVOCABLE,
      cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get()));

  // Kill the container and clean up.
  Future<Option<int>> status = process::reap(pid.get());

  AWAIT_READY(launcher.get()->destroy(containerId));

  AWAIT_READY(status);

  AWAIT_READY(isolator->cleanup(containerId));
}
#endif // __linux__


#ifdef __linux__
class LimitedCpuIsolatorTest : public MesosTest {};


TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_CFS_Enable_Cfs)
{
  slave::Flags flags;

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Try<Isolator*> _isolator = CgroupsCpushareIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  // Set the executor's resources to 0.5 cpu.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.5").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> prepare =
    isolator->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(prepare);

  // Generate random numbers to max out a single core. We'll run this for 0.5
  // seconds of wall time so it should consume approximately 250 ms of total
  // cpu time when limited to 0.5 cpu. We use /dev/urandom to prevent blocking
  // on Linux when there's insufficient entropy.
  string command = "cat /dev/urandom > /dev/null & "
    "export MESOS_TEST_PID=$! && "
    "sleep 0.5 && "
    "kill $MESOS_TEST_PID";

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  vector<Subprocess::Hook> parentHooks;

  // Create parent Hook to isolate child.
  //
  // NOTE: We can safely use references here as the hook will be executed
  // during the call to `fork`.
  const lambda::function<Try<Nothing>(pid_t)> isolatePidHook = lambda::bind(
      isolatePid,
      lambda::_1,
      std::cref(isolator),
      std::cref(containerId),
      NO_CHECK_CGROUPS);

  parentHooks.emplace_back(Subprocess::Hook(isolatePidHook));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      prepare.get().isSome() ? prepare.get().get().namespaces() : 0,
      parentHooks);

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Wait for the command to complete.
  AWAIT_READY(status);

  Future<ResourceStatistics> usage = isolator->usage(containerId);
  AWAIT_READY(usage);

  // Expect that no more than 300 ms of cpu time has been consumed. We also
  // check that at least 50 ms of cpu time has been consumed so this test will
  // fail if the host system is very heavily loaded. This behavior is correct
  // because under such conditions we aren't actually testing the CFS cpu
  // limiter.
  double cpuTime = usage.get().cpus_system_time_secs() +
                   usage.get().cpus_user_time_secs();

  EXPECT_GE(0.30, cpuTime);
  EXPECT_LE(0.05, cpuTime);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}


// This test verifies that we can successfully launch a container with
// a big (>= 10 cpus) cpu quota. This is to catch the regression
// observed in MESOS-1049.
// TODO(vinod): Revisit this if/when the isolator restricts the number
// of cpus that an executor can use based on the slave cpus.
TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_CFS_Big_Quota)
{
  slave::Flags flags;

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Try<Isolator*> _isolator = CgroupsCpushareIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  // Set the executor's resources to 100.5 cpu.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:100.5").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> prepare =
    isolator->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(prepare);

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = "exit 0";

  vector<Subprocess::Hook> parentHooks;

  // Create parent Hook to isolate child.
  // Note: We can safely use references here as we are sure that the life time
  // of the parent will exceed the child.
  const lambda::function<Try<Nothing>(pid_t)> isolatePidHook = lambda::bind(
      isolatePid,
      lambda::_1,
      std::cref(isolator),
      std::cref(containerId),
      NO_CHECK_CGROUPS);

  parentHooks.emplace_back(Subprocess::Hook(isolatePidHook));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      prepare.get().isSome() ? prepare.get().get().namespaces() : 0,
      parentHooks);

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Wait for the command to complete successfully.
  AWAIT_READY(status);
  ASSERT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}


// A test to verify the number of processes and threads in a
// container.
TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_Pids_and_Tids)
{
  slave::Flags flags;
  flags.cgroups_cpu_enable_pids_and_tids_count = true;

  Try<Isolator*> _isolator = CgroupsCpushareIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  Try<Launcher*> _launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(_launcher);
  Owned<Launcher> launcher(_launcher.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.5;mem:512").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> prepare =
    isolator->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(prepare);

  // Right after the creation of the cgroup, which happens in
  // 'prepare', we check that it is empty.
  Future<ResourceStatistics> usage = isolator->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(0U, usage.get().processes());
  EXPECT_EQ(0U, usage.get().threads());

  // Use these to communicate with the test process after it has
  // exec'd to make sure it is running.
  int inputPipes[2];
  ASSERT_NE(-1, ::pipe(inputPipes));

  int outputPipes[2];
  ASSERT_NE(-1, ::pipe(outputPipes));

  vector<string> argv(1);
  argv[0] = "cat";

  vector<Subprocess::Hook> parentHooks;

  // Create parent Hook to isolate child.
  // Note: We can safely use references here as we are sure that the life time
  // of the parent will exceed the child.
  const lambda::function<Try<Nothing>(pid_t)> isolatePidHook = lambda::bind(
      isolatePid,
      lambda::_1,
      std::cref(isolator),
      std::cref(containerId),
      CHECK_CGROUPS);

  parentHooks.emplace_back(Subprocess::Hook(isolatePidHook));

  Try<pid_t> pid = launcher->fork(
      containerId,
      "cat",
      argv,
      Subprocess::FD(inputPipes[0], Subprocess::IO::OWNED),
      Subprocess::FD(outputPipes[1], Subprocess::IO::OWNED),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      prepare.get().isSome() ? prepare.get().get().namespaces() : 0,
      parentHooks);

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Write to the test process and wait for an echoed result.
  // This observation ensures that the "cat" process has
  // completed its part of the exec() procedure and is now
  // executing normally.
  AWAIT_READY(io::write(inputPipes[1], "foo")
    .then([outputPipes]() -> Future<short> {
      return io::poll(outputPipes[0], io::READ);
    }));

  // Process count should be 1 since 'cat' is still idling.
  usage = isolator->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(1U, usage.get().processes());
  EXPECT_EQ(1U, usage.get().threads());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Clean up the extra pipes created for synchronization.
  EXPECT_SOME(os::close(inputPipes[1]));
  EXPECT_SOME(os::close(outputPipes[0]));

  // Wait for the command to complete.
  AWAIT_READY(status);

  // After the process is killed, the cgroup should be empty again.
  usage = isolator->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(0U, usage.get().processes());
  EXPECT_EQ(0U, usage.get().threads());

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}
#endif // __linux__


template <typename T>
class MemIsolatorTest : public MesosTest {};


typedef ::testing::Types<
    PosixMemIsolatorProcess,
#ifdef __linux__
    CgroupsMemIsolatorProcess,
#endif // __linux__
    tests::Module<Isolator, TestMemIsolator>> MemIsolatorTypes;


TYPED_TEST_CASE(MemIsolatorTest, MemIsolatorTypes);


TYPED_TEST(MemIsolatorTest, MemUsage)
{
  slave::Flags flags;

  Try<Isolator*> _isolator = TypeParam::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("mem:1024").get());

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

  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  // Set up the reaper to wait on the subprocess.
  Future<Option<int>> status = process::reap(helper.pid().get());

  // Isolate the subprocess.
  AWAIT_READY(isolator->isolate(containerId, helper.pid().get()));

  const Bytes allocation = Megabytes(128);
  EXPECT_SOME(helper.increaseRSS(allocation));

  Future<ResourceStatistics> usage = isolator->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage.get().mem_rss_bytes(), allocation.bytes());

  // Ensure the process is killed.
  helper.cleanup();

  // Make sure the subprocess was reaped.
  AWAIT_READY(status);

  // Let the isolator clean up.
  AWAIT_READY(isolator->cleanup(containerId));
}


#ifdef __linux__
class NetClsIsolatorTest : public MesosTest {};


// This tests the create, prepare, isolate and cleanup methods of the
// 'CgroupNetClsIsolatorProcess'. The test first creates a 'MesosContainerizer'
// with net_cls cgroup isolator enabled. The net_cls cgroup isolator is
// implemented in the 'CgroupNetClsIsolatorProcess' class. The test then
// launches a task in a mesos container and checks to see if the container has
// been added to the right net_cls cgroup. Finally, the test kills the task and
// makes sure that the 'CgroupNetClsIsolatorProcess' cleans up the net_cls
// cgroup created for the container.
TEST_F(NetClsIsolatorTest, ROOT_CGROUPS_NetClsIsolate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  uint16_t primary = 0x0012;

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/net_cls";
  flags.cgroups_net_cls_primary_handle = stringify(primary);
  flags.cgroups_net_cls_secondary_handles = "0xffff,0xffff";

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
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
  EXPECT_EQ(1u, offers.get().size());

  // Create a task to be launched in the mesos-container. We will be
  // explicitly killing this task to perform the cleanup test.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the update to verify that the task has been launched.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  // Task is ready.  Make sure there is exactly 1 container in the hashset.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers.get().size());

  const ContainerID& containerID = *(containers.get().begin());

  Result<string> hierarchy = cgroups::hierarchy("net_cls");
  ASSERT_SOME(hierarchy);

  // Check if the net_cls cgroup for this container exists, by
  // checking for the processes associated with this cgroup.
  string cgroup = path::join(
      flags.cgroups_root,
      containerID.value());

  Try<set<pid_t>> pids = cgroups::processes(hierarchy.get(), cgroup);
  ASSERT_SOME(pids);

  // There should be at least one TGID associated with this cgroup.
  EXPECT_LE(1u, pids.get().size());

  // Read the `net_cls.classid` to verify that the handle has been set.
  Try<uint32_t> classid = cgroups::net_cls::classid(hierarchy.get(), cgroup);
  EXPECT_SOME(classid);

  if (classid.isSome()) {
    // Make sure the primary handle is the same as the one set in
    // `--cgroup_net_cls_primary_handle`.
    EXPECT_EQ(primary, (classid.get() & 0xffff0000) >> 16);

    // Make sure the secondary handle is 0xffff.
    EXPECT_EQ(0xffffu, classid.get() & 0xffff);
  }

  // Isolator cleanup test: Killing the task should cleanup the cgroup
  // associated with the container.
  Future<TaskStatus> killStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&killStatus));

  // Wait for the executor to exit. We are using 'gc.schedule' as a proxy event
  // to monitor the exit of the executor.
  Future<Nothing> gcSchedule = FUTURE_DISPATCH(
      _, &slave::GarbageCollectorProcess::schedule);

  driver.killTask(status.get().task_id());

  AWAIT_READY(gcSchedule);

  // If the cleanup is successful the net_cls cgroup for this container should
  // not exist.
  ASSERT_FALSE(os::exists(cgroup));

  driver.stop();
  driver.join();
}


// This test verifies that we are able to retrieve the `net_cls` handle
// from `/state`.
TEST_F(NetClsIsolatorTest, ROOT_CGROUPS_ContainerStatus)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/net_cls";
  flags.cgroups_net_cls_primary_handle = stringify(0x0012);
  flags.cgroups_net_cls_secondary_handles = "0x0011,0x0012";

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
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
  EXPECT_EQ(1u, offers.get().size());

  // Create a task to be launched in the mesos-container.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  // Task is ready. Verify `ContainerStatus` is present in slave state.
  Future<Response> response = http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Object> netCls = parse->find<JSON::Object>(
      "frameworks[0].executors[0].tasks[0].statuses[0]."
      "container_status.cgroup_info.net_cls");
  ASSERT_SOME(netCls);

  uint32_t classid =
    netCls->values["classid"].as<JSON::Number>().as<uint32_t>();

  // Check the primary and the secondary handle.
  EXPECT_EQ(0x0012u, classid >> 16);
  EXPECT_EQ(0x0011u, classid & 0xffff);

  driver.stop();
  driver.join();
}
#endif


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
typedef ::testing::Types<
  CgroupsMemIsolatorProcess,
  CgroupsCpushareIsolatorProcess,
  CgroupsPerfEventIsolatorProcess> CgroupsIsolatorTypes;


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
