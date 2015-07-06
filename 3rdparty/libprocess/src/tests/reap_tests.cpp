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

using namespace process;

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
                               Fork(Exec("sleep 10")),
                               Exec("exit 0"))();
  ASSERT_SOME(tree);
  ASSERT_EQ(1u, tree.get().children.size());
  pid_t grandchild = tree.get().children.front();

  // Reap the grandchild process.
  Future<Option<int> > status = process::reap(grandchild);

  EXPECT_TRUE(status.isPending());

  // Now kill the grandchild.
  // NOTE: We send a SIGKILL here because sometimes the grandchild
  // process seems to be in a hung state and not responding to
  // SIGTERM/SIGINT.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));

  Clock::pause();

  // Now advance time until the Reaper reaps the grandchild.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(status);

  // The status is None because pid is not an immediate child.
  ASSERT_NONE(status.get()) << status.get().get();

  // Reap the child as well to clean up after ourselves.
  status = process::reap(tree.get().process.pid);

  // Now advance time until the child is reaped.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Check if the status is correct.
  ASSERT_SOME(status.get());
  int status_ = status.get().get();
  ASSERT_TRUE(WIFEXITED(status_));
  ASSERT_EQ(0, WEXITSTATUS(status_));

  Clock::resume();
}


// This test checks that the we can reap a child process and obtain
// the correct exit status.
TEST(ReapTest, ChildProcess)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // The child process sleeps and will be killed by the parent.
  Try<ProcessTree> tree = Fork(None(),
                               Exec("sleep 10"))();

  ASSERT_SOME(tree);
  pid_t child = tree.get();

  // Reap the child process.
  Future<Option<int> > status = process::reap(child);

  // Now kill the child.
  EXPECT_EQ(0, kill(child, SIGKILL));

  Clock::pause();

  // Now advance time until the reaper reaps the child.
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(status);

  // Check if the status is correct.
  ASSERT_SOME(status.get());
  int status_ = status.get().get();
  ASSERT_TRUE(WIFSIGNALED(status_));
  ASSERT_EQ(SIGKILL, WTERMSIG(status_));

  Clock::resume();
}


// Check that we can reap a child process that is already exited.
TEST(ReapTest, TerminatedChildProcess)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

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

    if (process.get().zombie) {
      break;
    }

    os::sleep(Milliseconds(1));
  }

  // Now that it's terminated, attempt to reap it.
  Future<Option<int> > status = process::reap(child);

  // Advance time until the reaper sends the notification.
  Clock::pause();
  while (status.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_READY(status);

  // Expect to get the correct status.
  ASSERT_SOME(status.get());

  int status_ = status.get().get();
  ASSERT_TRUE(WIFEXITED(status_));
  ASSERT_EQ(0, WEXITSTATUS(status_));

  Clock::resume();
}
