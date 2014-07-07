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

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/abort.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "master/master.hpp"
#include "master/detector.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/isolator.hpp"
#ifdef __linux__
#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"
#include "slave/containerizer/isolators/cgroups/perf_event.hpp"
#endif // __linux__
#include "slave/containerizer/isolators/posix.hpp"

#include "slave/containerizer/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#endif // __linux__

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;

using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Master;
#ifdef __linux__
using mesos::internal::slave::CgroupsCpushareIsolatorProcess;
using mesos::internal::slave::CgroupsMemIsolatorProcess;
using mesos::internal::slave::CgroupsPerfEventIsolatorProcess;
#endif // __linux__
using mesos::internal::slave::Isolator;
using mesos::internal::slave::IsolatorProcess;
using mesos::internal::slave::Launcher;
#ifdef __linux__
using mesos::internal::slave::LinuxLauncher;
#endif // __linux__
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::PosixCpuIsolatorProcess;
using mesos::internal::slave::PosixMemIsolatorProcess;


using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;


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

#ifdef __linux__
typedef ::testing::Types<PosixCpuIsolatorProcess,
                         CgroupsCpushareIsolatorProcess> CpuIsolatorTypes;
#else
typedef ::testing::Types<PosixCpuIsolatorProcess> CpuIsolatorTypes;
#endif // __linux__

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
  containerId.set_value("user_cpu_usage");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

  Try<string> dir = os::mkdtemp();
  ASSERT_SOME(dir);
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

  CHECK_SOME(os::rmdir(dir.get()));
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
  containerId.set_value("system_cpu_usage");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

  Try<string> dir = os::mkdtemp();
  ASSERT_SOME(dir);
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

  CHECK_SOME(os::rmdir(dir.get()));
}


#ifdef __linux__
class LimitedCpuIsolatorTest : public MesosTest {};

TEST_F(LimitedCpuIsolatorTest, ROOT_CGROUPS_Cfs)
{
  slave::Flags flags;

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Try<Isolator*> isolator = CgroupsCpushareIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources to 0.5 cpu.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:0.5").get());

  ContainerID containerId;
  containerId.set_value("mesos_test_cfs_cpu_limit");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

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

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources to 100.5 cpu.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("cpus:100.5").get());

  ContainerID containerId;
  containerId.set_value("mesos_test_cfs_big_cpu_limit");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

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

#endif // __linux__


template <typename T>
class MemIsolatorTest : public MesosTest {};

#ifdef __linux__
typedef ::testing::Types<PosixMemIsolatorProcess,
                         CgroupsMemIsolatorProcess> MemIsolatorTypes;
#else
typedef ::testing::Types<PosixMemIsolatorProcess> MemIsolatorTypes;
#endif // __linux__

TYPED_TEST_CASE(MemIsolatorTest, MemIsolatorTypes);


// This function should be async-signal-safe but it isn't: at least
// posix_memalign, mlock, memset and perror are not safe.
int consumeMemory(const Bytes& _size, const Duration& duration, int pipes[2])
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

  size_t size = static_cast<size_t>(_size.bytes());
  void* buffer = NULL;

  if (posix_memalign(&buffer, getpagesize(), size) != 0) {
    perror("Failed to allocate page-aligned memory, posix_memalign");
    abort();
  }

  // We use mlock and memset here to make sure that the memory
  // actually gets paged in and thus accounted for.
  if (mlock(buffer, size) != 0) {
    perror("Failed to lock memory, mlock");
    abort();
  }

  if (memset(buffer, 1, size) != buffer) {
    perror("Failed to fill memory, memset");
    abort();
  }

  os::sleep(duration);

  return 0;
}


TYPED_TEST(MemIsolatorTest, MemUsage)
{
  slave::Flags flags;

  Try<Isolator*> isolator = TypeParam::create(flags);
  CHECK_SOME(isolator);

  // A PosixLauncher is sufficient even when testing a cgroups isolator.
  Try<Launcher*> launcher = PosixLauncher::create(flags);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse("mem:1024").get());

  ContainerID containerId;
  containerId.set_value("memory_usage");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launcher.get()->fork(
      containerId,
      "/bin/sh",
      vector<string>(),
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      lambda::bind(&consumeMemory, Megabytes(256), Seconds(10), pipes));

  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ASSERT_SOME(os::close(pipes[0]));

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));

  ASSERT_SOME(os::close(pipes[1]));

  // Wait up to 5 seconds for the child process to consume 256 MB of memory;
  ResourceStatistics statistics;
  Bytes threshold = Megabytes(256);
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.mem_rss_bytes() >= threshold.bytes()) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < Seconds(5));

  EXPECT_LE(threshold.bytes(), statistics.mem_rss_bytes());

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
class PerfEventIsolatorTest : public MesosTest {};

TEST_F(PerfEventIsolatorTest, ROOT_CGROUPS_Sample)
{
  slave::Flags flags;

  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);

  Try<Isolator*> isolator = CgroupsPerfEventIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  ExecutorInfo executorInfo;

  ContainerID containerId;
  containerId.set_value("test");

  AWAIT_READY(isolator.get()->prepare(containerId, executorInfo));

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

#endif // __linux__
