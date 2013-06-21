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

#include <signal.h>
#include <unistd.h>

#include <sys/wait.h>

#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/exit.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "slave/reaper.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using process::Clock;
using process::Future;

using testing::_;
using testing::DoDefault;


// This test checks that the Reaper can monitor a non-child process.
TEST(ReaperTest, NonChildProcess)
{
  // Use pipes to determine the pid of the grand child process.
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));

  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    close(pipes[1]);

    // Get the grand child's pid via the pipe.
    ASSERT_NE(-1, read(pipes[0], &pid, sizeof(pid)));

    close(pipes[0]);
  } else {
    // In child process.
    close(pipes[0]);

    // Double fork!
    if ((pid = fork()) == -1) {
      perror("Failed to fork a grand child process");
      abort();
    }

    if (pid > 0) {
      // Still in child process.
      exit(0);
    } else {
      // In grand child process.

      // Ensure the parent process exits before we write the pid.
      while (getppid() != 1);

      pid = getpid();
      if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }
      close(pipes[1]);
      while (true); // Keep waiting till we get a signal.
    }
  }

  // In parent process.
  LOG(INFO) << "Grand child process " << pid;

  Reaper reaper;

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the grand child process.
  Future<Option<int> > status = reaper.monitor(pid);

  AWAIT_READY(monitor);

  // Now kill the grand child.
  // NOTE: We send a SIGKILL here because sometimes the grand child
  // process seems to be in a hung state and not responding to
  // SIGTERM/SIGINT.
  EXPECT_EQ(0, kill(pid, SIGKILL));

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Ensure the reaper notifies of the terminated process.
  AWAIT_READY(status);

  // Status is None because pid is not an immediate child.
  ASSERT_NONE(status.get());

  Clock::resume();
}


// This test checks that the Reaper can monitor a child process with
// accurate exit status returned.
TEST(ReaperTest, ChildProcess)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process. Keep waiting till we get a signal.
    while (true);
  }

  // In parent process.
  LOG(INFO) << "Child process " << pid;

  Reaper reaper;

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the grand child process.
  Future<Option<int> > status = reaper.monitor(pid);

  AWAIT_READY(monitor);

  // Now kill the child.
  EXPECT_EQ(0, kill(pid, SIGKILL));

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Ensure the reaper notifies of the terminated process.
  AWAIT_READY(status);

  // Check if the status is correct.
  ASSERT_SOME(status.get());
  int status_ = status.get().get();
  ASSERT_TRUE(WIFSIGNALED(status_));
  ASSERT_EQ(SIGKILL, WTERMSIG(status_));

  Clock::resume();
}


// Check that the Reaper can monitor a child process that exits
// before monitor() is called on it.
TEST(ReaperTest, TerminatedChildProcess)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process. Return directly
    exit(EXIT_SUCCESS);
  }

  // In parent process.
  LOG(INFO) << "Child process " << pid;

  Reaper reaper;

  Clock::pause();

  ASSERT_SOME(os::alive(pid));

  // Because reaper reaps all child processes even if they aren't
  // registered, we advance time until that happens.
  while (os::alive(pid).get()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Now we request to monitor the child process which is already
  // reaped.

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the child process.
  Future<Option<int> > status = reaper.monitor(pid);

  AWAIT_READY(monitor);

  // Now advance time until the reaper sends the notification.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Ensure the reaper notifies of the terminated process.
  AWAIT_READY(status);

  // Invalid status is returned because it is reaped before being
  // monitored.
  ASSERT_NONE(status.get());

  Clock::resume();
}
