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
#ifdef WITH_NETWORK_ISOLATOR
#include <stout/json.hpp>
#include <stout/net.hpp>
#endif // WITH_NETWORK_ISOLATOR
#include <stout/os.hpp>
#include <stout/path.hpp>

#ifdef WITH_NETWORK_ISOLATOR
#include "linux/routing/utils.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/ingress.hpp"
#endif // WITH_NETWORK_ISOLATOR

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
#ifdef WITH_NETWORK_ISOLATOR
#include "slave/containerizer/isolators/network/port_mapping.hpp"
#endif // WITH_NETWORK_ISOLATOR

#include "slave/containerizer/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#endif // __linux__

#ifdef WITH_NETWORK_ISOLATOR
#include "tests/flags.hpp"
#endif // WITH_NETWORK_ISOLATOR
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;

using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace process;
#ifdef WITH_NETWORK_ISOLATOR
using namespace routing;
using namespace routing::queueing;
#endif // WITH_NETWORK_ISOLATOR

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
#ifdef WITH_NETWORK_ISOLATOR
using mesos::internal::slave::MesosContainerizerLaunch;
using mesos::internal::slave::PortMappingIsolatorProcess;
#endif // WITH_NETWORK_ISOLATOR
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

#ifdef WITH_NETWORK_ISOLATOR
class PortMappingIsolatorTest : public TemporaryDirectoryTest
{
public:
  static void SetUpTestCase()
  {
    ASSERT_SOME(routing::check())
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because your\n"
      << "libnl library is not new enough. You can either install a\n"
      << "new libnl library, or disable this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME_EQ(0, os::shell(NULL, "which nc"))
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because 'nc'\n"
      << "could not be found. You can either install 'nc', or disable\n"
      << "this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME_EQ(0, os::shell(NULL, "which arping"))
      << "-------------------------------------------------------------\n"
      << "We cannot run some PortMappingIsolatorTests because 'arping'\n"
      << "could not be found. You can either isntall 'arping', or\n"
      << "disable this test case\n"
      << "-------------------------------------------------------------";
  }

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    flags = CreateSlaveFlags();

    // Guess the name of the public interface.
    Result<string> _eth0 = link::eth0();
    ASSERT_SOME(_eth0) << "Failed to guess the name of the public interface";

    eth0 = _eth0.get();

    LOG(INFO) << "Using " << eth0 << " as the public interface";

    // Guess the name of the loopback interface.
    Result<string> _lo = link::lo();
    ASSERT_SOME(_lo) << "Failed to guess the name of the loopback interface";

    lo = _lo.get();

    LOG(INFO) << "Using " << lo << " as the loopback interface";

    // Clean up qdiscs and veth devices.
    cleanup();

    // Get host IP address.
    Result<net::IP> _hostIP = net::ip(eth0);
    CHECK_SOME(_hostIP)
      << "Failed to retrieve the host public IP from " << eth0 << ": "
      << _hostIP.error();

    hostIP = _hostIP.get();

    // Get all the external name servers for tests that need to talk
    // to an external host, e.g., ping, DNS.
    Try<string> read = os::read("/etc/resolv.conf");
    CHECK_SOME(read);

    foreach (const string& line, strings::split(read.get(), "\n")) {
      if (!strings::startsWith(line, "nameserver")) {
        continue;
      }

      vector<string> tokens = strings::split(line, " ");
      ASSERT_EQ(2u, tokens.size()) << "Unexpected format in '/etc/resolv.conf'";
      if (tokens[1] != "127.0.0.1") {
        nameServers.push_back(tokens[1]);
      }
    }

    container1Ports = "ports:[31000-31499]";
    container2Ports = "ports:[31500-32000]";

    port = 31001;

    // errorPort is not in resources and private_resources.
    errorPort = 32502;

    container1Ready = path::join(os::getcwd(), "container1_ready");
    container2Ready = path::join(os::getcwd(), "container2_ready");
    trafficViaLoopback = path::join(os::getcwd(), "traffic_via_loopback");
    trafficViaPublic = path::join(os::getcwd(), "traffic_via_public");
    exitStatus = path::join(os::getcwd(), "exit_status");
  }

  virtual void TearDown()
  {
    cleanup();
    TemporaryDirectoryTest::TearDown();
  }

  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags;

    flags.launcher_dir = path::join(tests::flags.build_dir, "src");
    flags.resources = "cpus:2;mem:1024;disk:1024;ports:[31000-32000]";
    flags.isolation = "network/port_mapping";
    flags.private_resources = "ports:" + slave::DEFAULT_EPHEMERAL_PORTS;

    return flags;
  }

  void cleanup()
  {
    // Clean up the ingress qdisc on eth0 and lo if exists.
    Try<bool> hostEth0ExistsQdisc = ingress::exists(eth0);
    ASSERT_SOME(hostEth0ExistsQdisc);
    if (hostEth0ExistsQdisc.get()) {
      ASSERT_SOME_TRUE(ingress::remove(eth0));
    }

    Try<bool> hostLoExistsQdisc = ingress::exists(lo);
    ASSERT_SOME(hostLoExistsQdisc);
    if (hostLoExistsQdisc.get()) {
      ASSERT_SOME_TRUE(ingress::remove(lo));
    }

    // Clean up all 'veth' devices if exist.
    Try<set<string> > links = net::links();
    ASSERT_SOME(links);
    foreach (const string& name, links.get()) {
      if (strings::startsWith(name, slave::VETH_PREFIX)) {
        ASSERT_SOME_TRUE(link::remove(name));
      }
    }

    // TODO(chzhcn): Consider also removing bind mounts if exist.
  }

  Try<pid_t> launchHelper(
      Launcher* launcher,
      int pipes[2],
      const ContainerID& containerId,
      const string& command,
      const Option<CommandInfo>& preparation)
  {
    CommandInfo commandInfo;
    commandInfo.set_value(command);

    // The flags to pass to the helper process.
    MesosContainerizerLaunch::Flags launchFlags;

    launchFlags.command = JSON::Protobuf(commandInfo);
    launchFlags.directory = os::getcwd();

    CHECK_SOME(os::user());
    launchFlags.user = os::user().get();

    launchFlags.pipe_read = pipes[0];
    launchFlags.pipe_write = pipes[1];

    JSON::Object commands;
    JSON::Array array;
    array.values.push_back(JSON::Protobuf(preparation.get()));
    commands.values["commands"] = array;

    launchFlags.commands = commands;

    vector<string> argv(2);
    argv[0] = "mesos-containerizer";
    argv[1] = MesosContainerizerLaunch::NAME;

    Try<pid_t> pid = launcher->fork(
        containerId,
        path::join(flags.launcher_dir, "mesos-containerizer"),
        argv,
        Subprocess::FD(STDIN_FILENO),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO),
        launchFlags,
        None(),
        None());

    return pid;
  }

  slave::Flags flags;

  // Name of the host eth0 and lo.
  string eth0;
  string lo;

  // Host public IP.
  Option<net::IP> hostIP;

  // 'port' is within the range of ports assigned to one container.
  int port;

  // 'errorPort' is outside the range of ports assigned to the
  // container. Connecting to a container using this port will fail.
  int errorPort;

  // Ports assigned to container1.
  string container1Ports;

  // Ports assigned to container2.
  string container2Ports;

  // All the external name servers as read from /etc/resolv.conf.
  vector<string> nameServers;

  // Some auxiliary files for the tests.
  string container1Ready;
  string container2Ready;
  string trafficViaLoopback;
  string trafficViaPublic;
  string exitStatus;
};


// This test uses 2 containers: one listens to 'port' and 'errorPort'
// and writes data received to files; the other container attemptes to
// connect to the previous container using 'port' and
// 'errorPort'. Verify that only the connection through 'port' is
// successful.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerToContainerTCPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -l " << net::IP(hostIP.get().address()) << " " << port
           << " > " << trafficViaPublic << "& ";

  // Listen to 'errorPort'. This should not get anything.
  command1 << "nc -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;
  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. This should fail.
  command2 << "echo -n hello2 | nc localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc " << net::IP(hostIP.get().address())
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. This should fail.
  command2 << "echo -n hello4 | nc " << net::IP(hostIP.get().address())
           << " " << errorPort << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      preparation2.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// The same container-to-container test but with UDP.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerToContainerUDPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -u -l " << net::IP(hostIP.get().address()) << " " << port
           << " > " << trafficViaPublic << "& ";

  // Listen to 'errorPort'. This should not receive anything.
  command1 << "nc -u -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;
  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc -w1 -u localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello2 | nc -w1 -u localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc -w1 -u " << net::IP(hostIP.get().address())
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello4 | nc -w1 -u " << net::IP(hostIP.get().address())
           << " " << errorPort << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      preparation2.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a UDP server is in a container while host
// tries to establish a UDP connection.
TEST_F(PortMappingIsolatorTest, ROOT_HostToContainerUDPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'Port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -u -l " << net::IP(hostIP.get().address()) << " " << port
           << " > " << trafficViaPublic << "&";

  // Listen to 'public IP' and 'errorPort'. This should not receive anything.
  command1 << "nc -u -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "&";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  // Send to 'localhost' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello1 | nc -w1 -u localhost %s",
      stringify(port).c_str()));

  // Send to 'localhost' and 'errorPort'. The command should return
  // successfully because UDP is stateless but no data could be sent.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello2 | nc -w1 -u localhost %s",
      stringify(errorPort).c_str()));

  // Send to 'public IP' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello3 | nc -w1 -u %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. The command should return
  // successfully because UDP is stateless but no data could be sent.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello4 | nc -w1 -u %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(errorPort).c_str()));

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a TCP server is in a container while host
// tries to establish a TCP connection.
TEST_F(PortMappingIsolatorTest, ROOT_HostToContainerTCPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'Port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -l " << net::IP(hostIP.get().address()) << " " << port
           << " > " << trafficViaPublic << "&";

  // Listen to 'public IP' and 'errorPort'. This should fail.
  command1 << "nc -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "&";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  // Send to 'localhost' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello1 | nc localhost %s",
      stringify(port).c_str()));

  // Send to 'localhost' and 'errorPort'. This should fail because TCP
  // connection couldn't be established..
  ASSERT_SOME_EQ(256, os::shell(
      NULL,
      "echo -n hello2 | nc localhost %s",
      stringify(errorPort).c_str()));

  // Send to 'public IP' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello3 | nc %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. This should fail because TCP
  // connection couldn't be established.
  ASSERT_SOME_EQ(256, os::shell(
      NULL,
      "echo -n hello4 | nc %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(errorPort).c_str()));

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPExternalTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command1 << "ping -c1 " << IP;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to itself.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPInternalTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  command1 << "ping -c1 127.0.0.1 && ping -c1 "
           << stringify(net::IP(hostIP.get().address()));
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ARP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerARPExternalTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    // Time out after 1s and terminate upon receiving the first reply.
    command1 << "arping -f -w1 " << IP << " -I " << eth0;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test DNS connectivity.
TEST_F(PortMappingIsolatorTest, ROOT_DNSTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command1 << "host " << IP;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container has run out of ephemeral ports
// to use.
TEST_F(PortMappingIsolatorTest, ROOT_TooManyContainersTest)
{
  // Increase the ephemeral ports per container so that we dont have
  // enough ephemeral ports to launch a second container.
  flags.ephemeral_ports_per_container = 512;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  command1 << "sleep 1000";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_FAILED(preparation2);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete isolator.get();
  delete launcher.get();
}

#endif // WITH_NETWORK_ISOLATOR
