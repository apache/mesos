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
    // NOTE: This is also known as the System Idle Process.
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
  EXPECT_EQ(getpid(), process->pid);
  EXPECT_EQ(getppid(), process->parent);
  ASSERT_SOME(process->session);

#ifndef __WINDOWS__
  // NOTE: `getsid` does not have a meaningful interpretation on Windows.
  EXPECT_EQ(getsid(0), process->session.get());
#endif // __WINDOWS__

  ASSERT_SOME(process->rss);
  EXPECT_GT(process->rss.get(), 0);

  // NOTE: On Linux /proc is a bit slow to update the CPU times,
  // hence we allow 0 in this test.
  ASSERT_SOME(process->utime);
  EXPECT_GE(process->utime.get(), Nanoseconds(0));
  ASSERT_SOME(process->stime);
  EXPECT_GE(process->stime.get(), Nanoseconds(0));

  EXPECT_FALSE(process->command.empty());

  // Assert invalid PID returns `None`.
  Result<Process> invalid_process = os::process(-1);
  EXPECT_NONE(invalid_process);

  // Assert init.
  Result<Process> init_process = os::process(init_pid);
#ifdef __WINDOWS__
  // NOTE: On Windows, the init process is a pseudo-process, and it is not
  // possible to get a handle to it. So we expect it to error out instead of
  // succeed, unlike the POSIX version.
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
  ASSERT_GT(processes->size(), 2u);

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
      EXPECT_EQ(getsid(0), process.session.get());
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
  EXPECT_FALSE(pids->empty());
  EXPECT_EQ(1u, pids->count(getpid()));

  // In a FreeBSD jail, pid 1 may not exist.
#ifdef __FreeBSD__
  if (!isJailed()) {
#endif
#ifdef __WINDOWS__
    // On Windows, we explicitly do not return the PID of the System Idle
    // Process, because doing so can cause unexpected bugs.
    EXPECT_EQ(0u, pids.get().count(init_pid));
#else
    // Elsewhere we always expect exactly 1 init PID.
    EXPECT_EQ(1u, pids->count(init_pid));
#endif
#ifdef __FreeBSD__
  }
#endif

#ifndef __WINDOWS__
  // NOTE: `getpgid` does not have a meaningful interpretation on Windows.
  pids = os::pids(getpgid(0), None());
  EXPECT_SOME(pids);
  EXPECT_GE(pids->size(), 1u);
  EXPECT_EQ(1u, pids->count(getpid()));

  // NOTE: This test is not meaningful on Windows because process IDs are
  // expected to be non-negative.
  EXPECT_ERROR(os::pids(-1, None()));

  // NOTE: `getsid` does not have a meaningful interpretation on Windows.
  pids = os::pids(None(), getsid(0));
  EXPECT_SOME(pids);
  EXPECT_GE(pids->size(), 1u);
  EXPECT_EQ(1u, pids->count(getpid()));

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

  Try<os::windows::internal::ProcessData> process_data =
    os::windows::internal::create_process(
        "powershell",
        {"powershell", "-NoProfile", "-Command", "Start-Sleep", "2"},
        None());
  ASSERT_SOME(process_data);

  Try<ProcessTree> tree_after_spawn = os::pstree(getpid());
  ASSERT_SOME(tree_after_spawn);

  // Windows spawns conhost.exe if we're running from VS, so the count of
  // children could be 0 or 1.
  const size_t children_after_span = tree_after_spawn.get().children.size();
  EXPECT_TRUE((!conhost_spawned && 1u == children_after_span) ||
              (conhost_spawned && 2u == children_after_span)
              ) << stringify(tree_after_spawn.get());

  // Wait for the process synchronously.
  ::WaitForSingleObject(
      process_data.get().process_handle.get_handle(), INFINITE);
}
#else
TEST_F(ProcessTest, Pstree)
{
  Try<ProcessTree> tree = os::pstree(getpid());

  ASSERT_SOME(tree);
  EXPECT_TRUE(tree->children.empty()) << stringify(tree.get());

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
  ASSERT_LE(1u, tree->children.size());
  ASSERT_GE(2u, tree->children.size());

  pid_t child = tree->process.pid;
  pid_t grandchild = tree->children.front().process.pid;

  // Now check pstree again.
  tree = os::pstree(child);

  ASSERT_SOME(tree);
  EXPECT_EQ(child, tree->process.pid);

  ASSERT_LE(1u, tree->children.size());
  ASSERT_GE(2u, tree->children.size());

  // Cleanup by killing the descendant processes.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));
  EXPECT_EQ(0, kill(child, SIGKILL));

  // We have to reap the child for running the tests in repetition.
  ASSERT_EQ(child, waitpid(child, nullptr, 0));
}
#endif // __WINDOWS__
