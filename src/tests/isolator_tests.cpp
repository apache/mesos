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

#include <unistd.h>

#include <gmock/gmock.h>

#include <iostream>
#include <string>
#include <vector>

#include <mesos/resources.hpp>

#include <mesos/module/isolator.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/abort.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#include "linux/sched.hpp"
#endif // __linux__

#include "master/master.hpp"
#include "master/detector.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#ifdef __linux__
#include "slave/containerizer/isolators/cgroups/constants.hpp"
#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"
#include "slave/containerizer/isolators/cgroups/perf_event.hpp"
#include "slave/containerizer/isolators/filesystem/shared.hpp"
#endif // __linux__
#include "slave/containerizer/isolators/posix.hpp"

#include "slave/containerizer/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#endif // __linux__

#include "tests/flags.hpp"
#include "tests/memory_test_helper.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;
#ifdef __linux__
using mesos::internal::slave::CgroupsCpushareIsolatorProcess;
using mesos::internal::slave::CgroupsMemIsolatorProcess;
using mesos::internal::slave::CgroupsPerfEventIsolatorProcess;
using mesos::internal::slave::CPU_SHARES_PER_CPU_REVOCABLE;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::LinuxLauncher;
using mesos::internal::slave::SharedFilesystemIsolatorProcess;
#endif // __linux__
using mesos::internal::slave::Launcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::PosixCpuIsolatorProcess;
using mesos::internal::slave::PosixMemIsolatorProcess;
using mesos::internal::slave::Slave;

using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;

using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

static int childSetup(int pipes[2])
{
  // In child process.
  while (::close(pipes[1]) == -1 && errno == EINTR);

  // Wait until the parent signals us to continue.
  char dummy;
  ssize_t length;
  while ((length = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    ABORT("Failed to synchronize with parent");
  }

  while (::close(pipes[0]) == -1 && errno == EINTR);

  return 0;
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

  Try<Isolator*> isolator = TypeParam::create(flags);
  CHECK_SOME(isolator);

  // A PosixLauncher is sufficient even when testing a cgroups isolator.
  Try<Launcher*> launcher = PosixLauncher::create(flags);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1.0").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  const string& file = path::join(dir.get(), "mesos_isolator_test_ready");

  // Max out a single core in userspace. This will run for at most one second.
  string command = "while true ; do true ; done &"
    "touch " + file + "; " // Signals the command is running.
    "sleep 60";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&childSetup, pipes));

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Wait for the command to start.
  while (!os::exists(file));

  // Wait up to 1 second for the child process to induce 1/8 of a second of
  // user cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
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
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


TYPED_TEST(CpuIsolatorTest, SystemCpuUsage)
{
  slave::Flags flags;

  Try<Isolator*> isolator = TypeParam::create(flags);
  CHECK_SOME(isolator);

  // A PosixLauncher is sufficient even when testing a cgroups isolator.
  Try<Launcher*> launcher = PosixLauncher::create(flags);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1.0").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  const string& file = path::join(dir.get(), "mesos_isolator_test_ready");

  // Generating random numbers is done by the kernel and will max out a single
  // core and run almost exclusively in the kernel, i.e., system time.
  string command = "cat /dev/urandom > /dev/null & "
    "touch " + file + "; " // Signals the command is running.
    "sleep 60";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&childSetup, pipes));

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Wait for the command to start.
  while (!os::exists(file));

  // Wait up to 1 second for the child process to induce 1/8 of a second of
  // system cpu time.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
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
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


#ifdef __linux__
class RevocableCpuIsolatorTest : public MesosTest {};


TEST_F(RevocableCpuIsolatorTest, ROOT_CGROUPS_RevocableCpu)
{
  slave::Flags flags;

  Try<Isolator*> isolator = CgroupsCpushareIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = PosixLauncher::create(flags);

  // Include revocable CPU in the executor's resources.
  Resource cpu = Resources::parse("cpus", "1", "*").get();
  cpu.mutable_revocable();

  ExecutorInfo executorInfo;
  executorInfo.add_resources()->CopyFrom(cpu);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  AWAIT_READY(isolator.get()->prepare(
        containerId,
        executorInfo,
        os::getcwd(),
        None(),
        None()));

  vector<string> argv{"sleep", "100"};

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sleep",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      None(),
      None(),
      None());

  ASSERT_SOME(pid);

  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Check the executor has its scheduling policy set to IDLE.
  EXPECT_SOME_EQ(sched::Policy::IDLE, sched::policy::get(pid.get()));

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

  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}
#endif // __linux__


#ifdef __linux__
class LimitedCpuIsolatorTest : public MesosTest {};


TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_Cfs)
{
  slave::Flags flags;

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Try<Isolator*> isolator = CgroupsCpushareIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher =
    LinuxLauncher::create(flags, isolator.get()->namespaces().get());
  CHECK_SOME(launcher);

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

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  // Generate random numbers to max out a single core. We'll run this for 0.5
  // seconds of wall time so it should consume approximately 250 ms of total
  // cpu time when limited to 0.5 cpu. We use /dev/urandom to prevent blocking
  // on Linux when there's insufficient entropy.
  string command = "cat /dev/urandom > /dev/null & "
    "export MESOS_TEST_PID=$! && "
    "sleep 0.5 && "
    "kill $MESOS_TEST_PID";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&childSetup, pipes));

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Wait for the command to complete.
  AWAIT_READY(status);

  Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
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
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// This test verifies that we can successfully launch a container with
// a big (>= 10 cpus) cpu quota. This is to catch the regression
// observed in MESOS-1049.
// TODO(vinod): Revisit this if/when the isolator restricts the number
// of cpus that an executor can use based on the slave cpus.
TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_Cfs_Big_Quota)
{
  slave::Flags flags;

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Try<Isolator*> isolator = CgroupsCpushareIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher =
    LinuxLauncher::create(flags, isolator.get()->namespaces().get());
  CHECK_SOME(launcher);

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

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = "exit 0";

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&childSetup, pipes));

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Wait for the command to complete successfully.
  AWAIT_READY(status);
  ASSERT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// A test to verify the number of processes and threads in a
// container.
TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_Pids_and_Tids)
{
  slave::Flags flags;
  flags.cgroups_cpu_enable_pids_and_tids_count = true;

  Try<Isolator*> isolator = CgroupsCpushareIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher =
    LinuxLauncher::create(flags, isolator.get()->namespaces().get());
  CHECK_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.5;mem:512").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  // Right after the creation of the cgroup, which happens in
  // 'prepare', we check that it is empty.
  Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(0U, usage.get().processes());
  EXPECT_EQ(0U, usage.get().threads());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  vector<string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = "while true; do sleep 1; done;";

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&childSetup, pipes));

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Before isolation, the cgroup is empty.
  usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(0U, usage.get().processes());
  EXPECT_EQ(0U, usage.get().threads());

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // After the isolation, the cgroup is not empty, even though the
  // process hasn't exec'd yet.
  usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(1U, usage.get().processes());
  EXPECT_EQ(1U, usage.get().threads());

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Process count should be 1 since 'sleep' is still sleeping.
  usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(1U, usage.get().processes());
  EXPECT_EQ(1U, usage.get().threads());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Wait for the command to complete.
  AWAIT_READY(status);

  // After the process is killed, the cgroup should be empty again.
  usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_EQ(0U, usage.get().processes());
  EXPECT_EQ(0U, usage.get().threads());

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
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

  Try<Isolator*> isolator = TypeParam::create(flags);
  CHECK_SOME(isolator);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("mem:1024").get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  MemoryTestHelper helper;
  ASSERT_SOME(helper.spawn());
  ASSERT_SOME(helper.pid());

  // Set up the reaper to wait on the subprocess.
  Future<Option<int>> status = process::reap(helper.pid().get());

  // Isolate the subprocess.
  AWAIT_READY(isolator.get()->isolate(containerId, helper.pid().get()));

  const Bytes allocation = Megabytes(128);
  EXPECT_SOME(helper.increaseRSS(allocation));

  Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage.get().mem_rss_bytes(), allocation.bytes());

  // Ensure the process is killed.
  helper.cleanup();

  // Make sure the subprocess was reaped.
  AWAIT_READY(status);

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
}


#ifdef __linux__
class PerfEventIsolatorTest : public MesosTest {};


TEST_F(PerfEventIsolatorTest, ROOT_CGROUPS_Sample)
{
  slave::Flags flags;

  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);

  Try<Isolator*> isolator = CgroupsPerfEventIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  ExecutorInfo executorInfo;

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      dir.get(),
      None(),
      None()));

  // This first sample is likely to be empty because perf hasn't
  // completed yet but we should still have the required fields.
  Future<ResourceStatistics> statistics1 = isolator.get()->usage(containerId);
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
    Future<ResourceStatistics> statistics = isolator.get()->usage(containerId);
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

  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
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

  Try<Isolator*> isolator = SharedFilesystemIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher =
    LinuxLauncher::create(flags, isolator.get()->namespaces().get());
  CHECK_SOME(launcher);

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

  Future<Option<CommandInfo> > prepare =
    isolator.get()->prepare(
        containerId,
        executorInfo,
        flags.work_dir,
        None(),
        None());

  AWAIT_READY(prepare);
  ASSERT_SOME(prepare.get());

  // The test will touch a file in container path.
  const string file = path::join(containerPath, UUID::random().toString());
  ASSERT_FALSE(os::exists(file));

  // Manually run the isolator's preparation command first, then touch
  // the file.
  vector<string> args;
  args.push_back("/bin/sh");
  args.push_back("-x");
  args.push_back("-c");
  args.push_back(prepare.get().get().value() + " && touch " + file);

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      args,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      None());
  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int> > status = process::reap(pid.get());

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

  delete launcher.get();
  delete isolator.get();
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

  Try<Isolator*> isolator = SharedFilesystemIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher =
    LinuxLauncher::create(flags, isolator.get()->namespaces().get());
  CHECK_SOME(launcher);

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

  Future<Option<CommandInfo> > prepare =
    isolator.get()->prepare(
        containerId,
        executorInfo,
        flags.work_dir,
        None(),
        None());

  AWAIT_READY(prepare);
  ASSERT_SOME(prepare.get());

  // Test the volume mounting by touching a file in the container's
  // /tmp, which should then be in flags.work_dir.
  const string filename = UUID::random().toString();
  ASSERT_FALSE(os::exists(path::join(containerPath, filename)));

  vector<string> args;
  args.push_back("/bin/sh");
  args.push_back("-x");
  args.push_back("-c");
  args.push_back(prepare.get().get().value() +
                 " && touch " +
                 path::join(containerPath, filename));

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      args,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      None());
  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Check the file was created in flags.work_dir.
  EXPECT_TRUE(os::exists(path::join(hostPath, filename)));

  // Check it didn't get created in the host's view of containerPath.
  EXPECT_FALSE(os::exists(path::join(containerPath, filename)));

  delete launcher.get();
  delete isolator.get();
}


class NamespacesPidIsolatorTest : public MesosTest {};


TEST_F(NamespacesPidIsolatorTest, ROOT_PidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "namespaces/pid";

  string directory = os::getcwd(); // We're inside a temporary sandbox.

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Write the command's pid namespace inode and init name to files.
  const string command =
    "stat -c %i /proc/self/ns/pid > ns && (cat /proc/1/comm > init)";

  process::Future<bool> launch = containerizer.get()->launch(
      containerId,
      CREATE_EXECUTOR_INFO("executor", command),
      directory,
      None(),
      SlaveID(),
      process::PID<Slave>(),
      false);
  AWAIT_READY(launch);
  ASSERT_TRUE(launch.get());

  // Wait on the container.
  process::Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);
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

  delete containerizer.get();
}


// Username for the unprivileged user that will be created to test
// unprivileged cgroup creation. It will be removed after the tests.
// It is presumed this user does not normally exist.
const string UNPRIVILEGED_USERNAME = "mesos.test.unprivileged.user";


template <typename T>
class UserCgroupIsolatorTest : public MesosTest
{
public:
  static void SetUpTestCase()
  {
    // Remove the user in case it wasn't cleaned up from a previous
    // test.
    os::system("userdel -r " + UNPRIVILEGED_USERNAME + " > /dev/null");

    ASSERT_EQ(0, os::system("useradd " + UNPRIVILEGED_USERNAME));
  }


  static void TearDownTestCase()
  {
    ASSERT_EQ(0, os::system("userdel -r " + UNPRIVILEGED_USERNAME));
  }
};


// Test all isolators that use cgroups.
typedef ::testing::Types<
  CgroupsMemIsolatorProcess,
  CgroupsCpushareIsolatorProcess,
  CgroupsPerfEventIsolatorProcess> CgroupsIsolatorTypes;


TYPED_TEST_CASE(UserCgroupIsolatorTest, CgroupsIsolatorTypes);


TYPED_TEST(UserCgroupIsolatorTest, ROOT_CGROUPS_UserCgroup)
{
  slave::Flags flags;
  flags.perf_events = "cpu-cycles"; // Needed for CgroupsPerfEventIsolator.

  Try<Isolator*> isolator = TypeParam::create(flags);
  ASSERT_SOME(isolator);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("mem:1024;cpus:1").get()); // For cpu/mem isolators.

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  AWAIT_READY(isolator.get()->prepare(
      containerId,
      executorInfo,
      os::getcwd(),
      None(),
      UNPRIVILEGED_USERNAME));

  // Isolators don't provide a way to determine the cgroups they use
  // so we'll inspect the cgroups for an isolated dummy process.
  pid_t pid = fork();
  if (pid == 0) {
    // Child just sleeps.
    ::sleep(100);

    ABORT("Child process should not reach here");
  }
  ASSERT_GT(pid, 0);

  AWAIT_READY(isolator.get()->isolate(containerId, pid));

  // Get the container's cgroups from /proc/$PID/cgroup. We're only
  // interested in the cgroups path that is setup by the isolator,
  // which sets up cgroup under /mesos.
  // 6:blkio:/user.slice
  // 5:perf_event:/
  // 4:memory:/
  // 3:freezer:/
  // 2:cpuacct:/mesos
  // 1:cpu:/mesos
  // awk will then output "cpuacct/mesos\ncpu/mesos" as the cgroup(s).
  ostringstream output;
  Try<int> status = os::shell(
      &output,
      "grep '/mesos' /proc/" +
      stringify(pid) +
      "/cgroup | awk -F ':' '{print $2$3}'");

  ASSERT_SOME(status);

  // Kill the dummy child process.
  ::kill(pid, SIGKILL);
  int exitStatus;
  EXPECT_NE(-1, ::waitpid(pid, &exitStatus, 0));

  vector<string> cgroups = strings::tokenize(output.str(), "\n");
  ASSERT_FALSE(cgroups.empty());

  foreach (const string& cgroup, cgroups) {
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
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
