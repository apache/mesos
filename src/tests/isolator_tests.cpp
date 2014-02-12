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

#include <string>
#include <vector>

#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "master/master.hpp"
#include "master/detector.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"

#include "slave/containerizer/isolators/posix.hpp"
#ifdef __linux__
#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"
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
#endif // __linux__
using mesos::internal::slave::Isolator;
using mesos::internal::slave::IsolatorProcess;
using mesos::internal::slave::Launcher;
using mesos::internal::slave::PosixLauncher;
using mesos::internal::slave::PosixCpuIsolatorProcess;
using mesos::internal::slave::PosixMemIsolatorProcess;
using mesos::internal::slave::Flags;

using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;


int execute(const std::string& command, int pipes[2])
{
  // In child process
  ::close(pipes[1]);

  // Wait until the parent signals us to continue.
  int buf;
  while (::read(pipes[0], &buf, sizeof(buf)) == -1 && errno == EINTR);
  ::close(pipes[0]);

  execl("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL);

  std::cerr << "Should not reach here!" << std::endl;
  abort();
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
  Flags flags;

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

  lambda::function<int()> inChild = lambda::bind(&execute, command, pipes);

  Try<pid_t> pid = launcher.get()->fork(containerId, inChild);
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  int buf;
  ASSERT_LT(0, ::write(pipes[1],  &buf, sizeof(buf)));
  ::close(pipes[1]);

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

  // Shouldn't be any appreciable system time.
  EXPECT_GT(0.025, statistics.cpus_system_time_secs());

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
  Flags flags;

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

  lambda::function<int()> inChild = lambda::bind(&execute, command, pipes);

  Try<pid_t> pid = launcher.get()->fork(containerId, inChild);
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  int buf;
  ASSERT_LT(0, ::write(pipes[1],  &buf, sizeof(buf)));
  ::close(pipes[1]);

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

  // Shouldn't be any appreciable user time.
  EXPECT_GT(0.025, statistics.cpus_user_time_secs());

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
  // In child process
  ::close(pipes[1]);

  int buf;
  // Wait until the parent signals us to continue.
  while (::read(pipes[0], &buf, sizeof(buf)) == -1 && errno == EINTR);
  ::close(pipes[0]);

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
  Flags flags;

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

  lambda::function<int()> inChild = lambda::bind(
      &consumeMemory,
      Megabytes(256),
      Seconds(10),
      pipes);

  Try<pid_t> pid = launcher.get()->fork(containerId, inChild);
  ASSERT_SOME(pid);

  // Set up the reaper to wait on the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  int buf;
  ASSERT_LT(0, ::write(pipes[1], &buf, sizeof(buf)));
  ::close(pipes[1]);

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
