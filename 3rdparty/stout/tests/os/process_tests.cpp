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

#ifndef __WINDOWS__
#include <stout/os/fork.hpp>
#endif // __WINDOWS__
#include <stout/os/pstree.hpp>

#include <stout/tests/utils.hpp>


class ProcessTest : public TemporaryDirectoryTest {};

#ifndef __WINDOWS__
using os::Exec;
using os::Fork;
#endif // __WINDOWS__
using os::Process;
using os::ProcessTree;

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

  return -1;
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
#elif __FreeBSD__
  // In a FreeBSD jail, we wont find an init process.
  if (!isJailed()) {
      EXPECT_SOME(init_process);
  } else {
      EXPECT_NONE(init_process);
  }
#else
  EXPECT_SOME(init_process);
#endif // __WINDOWS__
}


TEST_F(ProcessTest, Processes)
{
  const Try<list<Process>> processes = os::processes();

  ASSERT_SOME(processes);
  ASSERT_GT(processes.get().size(), 2u);

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
  Try<set<pid_t>> pids = os::pids();
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


#ifdef __WINDOWS__
TEST_F(ProcessTest, Pstree)
{
  Try<ProcessTree> tree = os::pstree(getpid());
  ASSERT_SOME(tree);

  // Windows spawns `conhost.exe` if we're running from VS, so the count of
  // children could be 0 or 1.
  const size_t total_children = tree.get().children.size();
  EXPECT_TRUE(0u == total_children ||
              1u == total_children) << stringify(tree.get());
  const bool conhost_spawned = total_children == 1;

  // Windows has no `sleep` command, so we fake it with `ping`.
  const string command = "ping 127.0.0.1 -n 2";

  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  // Create new process that "sleeps".
  BOOL created = CreateProcess(
      nullptr,                // No module name (use command line).
      (LPSTR)command.c_str(),
      nullptr,                // Process handle not inheritable.
      nullptr,                // Thread handle not inheritable.
      FALSE,                  // Set handle inheritance to FALSE.
      0,                      // No creation flags.
      nullptr,                // Use parent's environment block.
      nullptr,                // Use parent's starting directory.
      &si,
      &pi);
  ASSERT_TRUE(created == TRUE);

  Try<ProcessTree> tree_after_spawn = os::pstree(getpid());
  ASSERT_SOME(tree_after_spawn);

  // Windows spawns conhost.exe if we're running from VS, so the count of
  // children could be 0 or 1.
  const size_t children_after_span = tree_after_spawn.get().children.size();
  EXPECT_TRUE((!conhost_spawned && 1u == children_after_span) ||
              (conhost_spawned && 2u == children_after_span)
              ) << stringify(tree_after_spawn.get());

  WaitForSingleObject(pi.hProcess, INFINITE);
}
#else
TEST_F(ProcessTest, Pstree)
{
  Try<ProcessTree> tree = os::pstree(getpid());

  ASSERT_SOME(tree);
  EXPECT_EQ(0u, tree.get().children.size()) << stringify(tree.get());

  tree =
    Fork(None(),                   // Child.
      Fork(Exec(SLEEP_COMMAND(10))),   // Grandchild.
      Exec(SLEEP_COMMAND(10)))();

  ASSERT_SOME(tree);

  // Depending on whether or not the shell has fork/exec'ed,
  // we could have 1 or 2 direct children. That is, some shells
  // might simply exec the command above (i.e., 'sleep 10') while
  // others might fork/exec the command, keeping around a 'sh -c'
  // process as well.
  ASSERT_LE(1u, tree.get().children.size());
  ASSERT_GE(2u, tree.get().children.size());

  pid_t child = tree.get().process.pid;
  pid_t grandchild = tree.get().children.front().process.pid;

  // Now check pstree again.
  tree = os::pstree(child);

  ASSERT_SOME(tree);
  EXPECT_EQ(child, tree.get().process.pid);

  ASSERT_LE(1u, tree.get().children.size());
  ASSERT_GE(2u, tree.get().children.size());

  // Cleanup by killing the descendant processes.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));
  EXPECT_EQ(0, kill(child, SIGKILL));

  // We have to reap the child for running the tests in repetition.
  ASSERT_EQ(child, waitpid(child, nullptr, 0));
}
#endif // __WINDOWS__
