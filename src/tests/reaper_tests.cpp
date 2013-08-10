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

using namespace os;
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
  // The child process creates a grandchild and then exits. The
  // grandchild sleeps for 10 seconds. The process tree looks like:
  //  -+- child exit 0
  //   \-+- grandchild sleep 10

  // After the child exits, the grandchild is going to be re-parented
  // by 'init', like this:
  //  -+- child (exit 0)
  //  -+- grandchild sleep 10
  Try<ProcessTree> tree = Fork(None(),
                               Fork(Exec("sleep 10")),
                               Exec("exit 0"))();
  ASSERT_SOME(tree);
  ASSERT_EQ(1u, tree.get().children.size());
  pid_t grandchild = tree.get().children.front();

  Reaper reaper;

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the grand child process.
  Future<Option<int> > status = reaper.monitor(grandchild);

  AWAIT_READY(monitor);

  // This makes sure the status only becomes ready after the
  // grandchild is killed.
  EXPECT_TRUE(status.isPending());

  // Now kill the grand child.
  // NOTE: We send a SIGKILL here because sometimes the grand child
  // process seems to be in a hung state and not responding to
  // SIGTERM/SIGINT.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));

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

  // The child process sleeps and will be killed by the parent.
  Try<ProcessTree> tree = Fork(None(),
                               Exec("sleep 10"))();

  ASSERT_SOME(tree);
  pid_t child = tree.get();

  Reaper reaper;

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the grand child process.
  Future<Option<int> > status = reaper.monitor(child);

  AWAIT_READY(monitor);

  // Now kill the child.
  EXPECT_EQ(0, kill(child, SIGKILL));

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

  // The child process immediately exits.
  Try<ProcessTree> tree = Fork(None(),
                               Exec("exit 0"))();

  ASSERT_SOME(tree);
  pid_t child = tree.get();

  ASSERT_SOME(os::process(child));

  Clock::pause();

  Reaper reaper;

  // Because reaper reaps all child processes even if they aren't
  // registered, we advance time until that happens.
  while (os::process(child).isSome()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Now we request to monitor the child process which is already
  // reaped.

  Future<Nothing> monitor = FUTURE_DISPATCH(_, &ReaperProcess::monitor);

  // Ask the reaper to monitor the child process.
  Future<Option<int> > status = reaper.monitor(child);

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
