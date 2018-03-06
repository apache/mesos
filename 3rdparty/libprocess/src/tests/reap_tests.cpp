// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <signal.h>
#include <unistd.h>

#include <sys/wait.h>

#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/reap.hpp>

#include <stout/exit.hpp>
#include <stout/gtest.hpp>
#include <stout/os/fork.hpp>
#include <stout/os/pstree.hpp>
#include <stout/try.hpp>

using process::Clock;
using process::Future;
using process::MAX_REAP_INTERVAL;

using os::Exec;
using os::Fork;
using os::ProcessTree;

using testing::_;
using testing::DoDefault;


// This test checks that we can reap a non-child process, in terms
// of receiving the termination notification.
TEST(ReapTest, NonChildProcess)
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
                               Fork(Exec(SLEEP_COMMAND(10))),
                               Exec("exit 0"))();
  ASSERT_SOME(tree);
  ASSERT_EQ(1u, tree->children.size());
  pid_t grandchild = tree->children.front();

  // Reap the grandchild process.
  Future<Option<int>> status = process::reap(grandchild);

  EXPECT_TRUE(status.isPending());

  // Now kill the grandchild.
  // NOTE: We send a SIGKILL here because sometimes the grandchild
  // process seems to be in a hung state and not responding to
  // SIGTERM/SIGINT.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));

  Clock::pause();

  // Now advance time until the Reaper reaps the grandchild.
  while (status.isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_READY(status);

  // The status is None because pid is not an immediate child.
  ASSERT_NONE(status.get()) << status->get();

  // Reap the child as well to clean up after ourselves.
  status = process::reap(tree->process.pid);

  // Now advance time until the child is reaped.
  while (status.isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Check if the status is correct.
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, status);

  Clock::resume();
}


// This test checks that the we can reap a child process and obtain
// the correct exit status.
TEST(ReapTest, ChildProcess)
{
  // The child process sleeps and will be killed by the parent.
  Try<ProcessTree> tree = Fork(None(),
                               Exec("sleep 10"))();

  ASSERT_SOME(tree);
  pid_t child = tree.get();

  // Reap the child process.
  Future<Option<int>> status = process::reap(child);

  // Now kill the child.
  EXPECT_EQ(0, kill(child, SIGKILL));

  Clock::pause();

  // Now advance time until the reaper reaps the child.
  while (status.isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Check if the status is correct.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, status);

  Clock::resume();
}


// Check that we can reap a child process that is already exited.
TEST(ReapTest, TerminatedChildProcess)
{
  // The child process immediately exits.
  Try<ProcessTree> tree = Fork(None(),
                               Exec("exit 0"))();

  ASSERT_SOME(tree);
  pid_t child = tree.get();

  ASSERT_SOME(os::process(child));

  // Make sure the process is transitioned into the zombie
  // state before we reap it.
  while (true) {
    const Result<os::Process> process = os::process(child);
    ASSERT_SOME(process) << "Process " << child << " reaped unexpectedly";

    if (process->zombie) {
      break;
    }

    os::sleep(Milliseconds(1));
  }

  // Now that it's terminated, attempt to reap it.
  Future<Option<int>> status = process::reap(child);

  // Advance time until the reaper sends the notification.
  Clock::pause();
  while (status.isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }

  // Expect to get the correct status.
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, status);

  Clock::resume();
}
