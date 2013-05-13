#include <unistd.h> // For getpid, getppid.

#include <gmock/gmock.h>

#include <set>

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
  Try<ProcessStatus> status = proc::status(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status.get().pid);
  EXPECT_EQ(getppid(), status.get().ppid);
}
