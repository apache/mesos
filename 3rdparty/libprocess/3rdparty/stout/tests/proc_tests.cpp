#include <iostream>

#include <unistd.h> // For getpid, getppid.

#include <gmock/gmock.h>

#include <set>

#include <stout/abort.hpp>
#include <stout/gtest.hpp>
#include <stout/proc.hpp>
#include <stout/try.hpp>

using proc::CPU;
using proc::SystemStatus;
using proc::ProcessStatus;

using std::set;


TEST(ProcTest, pids)
{
  Try<set<pid_t> > pids = proc::pids();

  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));
}


TEST(ProcTest, cpus)
{
  Try<std::list<CPU> > cpus = proc::cpus();

  ASSERT_SOME(cpus);
  EXPECT_LE(1u, cpus.get().size());
}


TEST(ProcTest, SystemStatus)
{
  Try<SystemStatus> status = proc::status();

  ASSERT_SOME(status);
  EXPECT_NE(0u, status.get().btime);
}


TEST(ProcTest, ProcessStatus)
{
  Result<ProcessStatus> status = proc::status(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status.get().pid);
  EXPECT_EQ(getppid(), status.get().ppid);
}


TEST(ProcTest, SingleThread)
{
  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process, wait until killed.
    while (true) { sleep(1); }

    // Should not reach here.
    ABORT("Error, child should be killed before reaching here");
  }

  // In parent process.
  // Check we have the expected number of threads.
  Try<set<pid_t> > threads = proc::threads(pid);

  ASSERT_SOME(threads);
  EXPECT_EQ(1u, threads.get().size());
  EXPECT_EQ(1u, threads.get().count(pid));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  int status;
  EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}


int threadFunction(void*)
{
  while (true) { sleep(1); }

  return -1;
}


TEST(ProcTest, MultipleThreads)
{
  int ready;
  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process
    ::close(pipes[0]);

    int numThreads = 5;

    // 1 MiB stack for each thread.
    size_t stackSize = 1024*1024 / sizeof(unsigned long long);
    unsigned long long stack[numThreads][stackSize];

    std::set<pid_t> threads;

    for (int i = 0; i < numThreads; i++) {
      pid_t thread;

      // We use clone here to create threads because pthread_create is not
      // async-signal-safe.
      thread = clone(
          threadFunction,
          &(stack[i][stackSize - 1]),
          CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | CLONE_FILES,
          NULL);

      EXPECT_NE(-1, thread);

      threads.insert(thread);
    }

    // Also add our own pid to the set.
    threads.insert(getpid());

    // Notify parent of the thread ids.
    foreach (const pid_t& thread, threads) {
      ssize_t len;
      while ((len = ::write(pipes[1], &thread, sizeof(thread))) == -1 && errno == EINTR);

      EXPECT_EQ(sizeof(thread), len);
    }

    // NOTE: CLONE_FILES ensures the pipe file descriptor will be closed in the
    // threads as well, ensuring the parent gets the EOF.
    ::close(pipes[1]);

    // Sleep until killed.
    while (true) { sleep(1); }

    // Should not reach here.
    ABORT("Error, child should be killed before reaching here");
  }

  // In parent process.
  ::close(pipes[1]);

  // Get thread ids from the child.
  std::set<pid_t> childThreads;

  ssize_t len;
  do {
    pid_t thread;

    while ((len = ::read(pipes[0], &thread, sizeof(thread))) == -1 && errno == EINTR);

    childThreads.insert(thread);
  } while (len > 0);

  ::close(pipes[0]);

  // Read thread ids from /proc for the child.
  Try<set<pid_t> > procThreads = proc::threads(pid);
  ASSERT_SOME(procThreads);

  //Check we have the expected threads.
  ASSERT_SOME_EQ(childThreads, procThreads);

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  int status;
  EXPECT_NE(-1, ::waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}
