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

#ifdef __WINDOWS__
#include <process.h>
#endif // __WINDOWS__

#include <set>

#include <gtest/gtest.h>

#include <stout/os.hpp>

#include <stout/tests/utils.hpp>


class ProcessTest : public TemporaryDirectoryTest {};

using os::Process;

using std::list;
using std::set;
using std::string;


const unsigned int init_pid =
#ifdef __WINDOWS__
    0;
#else
    1;
#endif // __WINDOWS__


#ifdef __WINDOWS__
int getppid()
{
  const int pid = getpid();
  HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  std::shared_ptr<void> sh(h, CloseHandle);

  PROCESSENTRY32 pe = { 0 };
  pe.dwSize = sizeof(PROCESSENTRY32);

  if (Process32First(h, &pe)) {
    do {
      if (pe.th32ProcessID == pid) {
        return pe.th32ParentProcessID;
      }
    } while (Process32Next(h, &pe));
  }
}
#endif // __WINDOWS__

TEST_F(ProcessTest, Process)
{
  const Result<Process> process = os::process(getpid());

  ASSERT_SOME(process);
  EXPECT_EQ(getpid(), process.get().pid);
  EXPECT_EQ(getppid(), process.get().parent);
  ASSERT_SOME(process.get().session);

#ifndef __WINDOWS__
  // NOTE: `getsid` does not have a meaningful interpretation on Windows.
  EXPECT_EQ(getsid(getpid()), process.get().session.get());
#endif // __WINDOWS__

  ASSERT_SOME(process.get().rss);
  EXPECT_GT(process.get().rss.get(), 0);

  // NOTE: On Linux /proc is a bit slow to update the CPU times,
  // hence we allow 0 in this test.
  ASSERT_SOME(process.get().utime);
  EXPECT_GE(process.get().utime.get(), Nanoseconds(0));
  ASSERT_SOME(process.get().stime);
  EXPECT_GE(process.get().stime.get(), Nanoseconds(0));

  EXPECT_FALSE(process.get().command.empty());

  // Assert invalid PID returns `None`.
  Result<Process> invalid_process = os::process(-1);
  EXPECT_NONE(invalid_process);

  // Assert init.
  Result<Process> init_process = os::process(init_pid);
#ifdef __WINDOWS__
  // NOTE: On Windows, inspecting other processes usually requires privileges.
  // So we expect it to error out instead of succeed, unlike the POSIX version.
  EXPECT_ERROR(init_process);
#else
  EXPECT_SOME(init_process);
#endif // __WINDOWS__
}


TEST_F(ProcessTest, Processes)
{
  const Try<list<Process>> processes = os::processes();

  ASSERT_SOME(processes);
  ASSERT_GT(processes.get().size(), 2);

  // Look for ourselves in the table.
  bool found = false;
  foreach (const Process& process, processes.get()) {
    if (process.pid == getpid()) {
      found = true;
      EXPECT_EQ(getpid(), process.pid);
      EXPECT_EQ(getppid(), process.parent);
      ASSERT_SOME(process.session);

#ifndef __WINDOWS__
      // NOTE: `getsid` does not have a meaningful interpretation on Windows.
      EXPECT_EQ(getsid(getpid()), process.session.get());
#endif // __WINDOWS__

      ASSERT_SOME(process.rss);
      EXPECT_GT(process.rss.get(), 0);

      // NOTE: On linux /proc is a bit slow to update the cpu times,
      // hence we allow 0 in this test.
      ASSERT_SOME(process.utime);
      EXPECT_GE(process.utime.get(), Nanoseconds(0));
      ASSERT_SOME(process.stime);
      EXPECT_GE(process.stime.get(), Nanoseconds(0));

      EXPECT_FALSE(process.command.empty());

      break;
    }
  }

  EXPECT_TRUE(found);
}


TEST_F(ProcessTest, Pids)
{
  Try<set<pid_t> > pids = os::pids();
  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));

  // In a FreeBSD jail, pid 1 may not exist.
#ifdef __FreeBSD__
  if (!isJailed()) {
#endif
    EXPECT_EQ(1u, pids.get().count(init_pid));
#ifdef __FreeBSD__
  }
#endif

#ifndef __WINDOWS__
  // NOTE: `getpgid` does not have a meaningful interpretation on Windows.
  pids = os::pids(getpgid(0), None());
  EXPECT_SOME(pids);
  EXPECT_GE(pids.get().size(), 1u);
  EXPECT_EQ(1u, pids.get().count(getpid()));

  // NOTE: This test is not meaningful on Windows because process IDs are
  // expected to be non-negative.
  EXPECT_ERROR(os::pids(-1, None()));

  // NOTE: `getsid` does not have a meaningful interpretation on Windows.
  pids = os::pids(None(), getsid(0));
  EXPECT_SOME(pids);
  EXPECT_GE(pids.get().size(), 1u);
  EXPECT_EQ(1u, pids.get().count(getpid()));

  // NOTE: This test is not meaningful on Windows because process IDs are
  // expected to be non-negative.
  EXPECT_ERROR(os::pids(None(), -1));
#endif // __WINDOWS__
}
