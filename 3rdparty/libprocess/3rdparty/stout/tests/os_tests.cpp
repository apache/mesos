#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stdint.h>

#ifndef __linux__
#include <sys/time.h> // For gettimeofday.
#endif

#include <cstdlib> // For rand.
#include <list>
#include <set>
#include <sstream>
#include <string>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#ifdef __APPLE__
#include <stout/os/sysctl.hpp>
#endif

#include <stout/tests/utils.hpp>

using os::Exec;
using os::Fork;
using os::Process;
using os::ProcessTree;

using std::list;
using std::set;
using std::string;
using std::vector;


static hashset<string> listfiles(const string& directory)
{
  hashset<string> fileset;
  foreach (const string& file, os::ls(directory)) {
    fileset.insert(file);
  }
  return fileset;
}


class OsTest : public TemporaryDirectoryTest {};


TEST_F(OsTest, environment)
{
  // Make sure the environment has some entries with '=' in the value.
  os::setenv("SOME_SPECIAL_FLAG", "--flag=foobar");

  char** environ = os::environ();

  hashmap<string, string> environment = os::environment();

  for (size_t index = 0; environ[index] != NULL; index++) {
    std::string entry(environ[index]);
    size_t position = entry.find_first_of('=');
    if (position == std::string::npos) {
      continue; // Skip malformed environment entries.
    }
    const string& key = entry.substr(0, position);
    const string& value = entry.substr(position + 1);
    EXPECT_TRUE(environment.contains(key));
    EXPECT_EQ(value, environment[key]);
  }
}


TEST_F(OsTest, rmdir)
{
  const hashset<string> EMPTY;
  const string& tmpdir = os::getcwd();

  hashset<string> expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  os::mkdir(tmpdir + "/a/b/c");
  os::mkdir(tmpdir + "/a/b/d");
  os::mkdir(tmpdir + "/e/f");

  expectedListing = EMPTY;
  expectedListing.insert("a");
  expectedListing.insert("e");
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  expectedListing = EMPTY;
  expectedListing.insert("b");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a"));

  expectedListing = EMPTY;
  expectedListing.insert("c");
  expectedListing.insert("d");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b"));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/c"));
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/a/b/d"));

  expectedListing.insert("f");
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e"));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir + "/e/f"));
}


TEST_F(OsTest, system)
{
  EXPECT_EQ(0, os::system("exit 0"));
  EXPECT_EQ(0, os::system("sleep 0"));
  EXPECT_NE(0, os::system("exit 1"));
  EXPECT_NE(0, os::system("invalid.command"));

  // Note that ::system returns 0 for the following two cases as well.
  EXPECT_EQ(0, os::system(""));
  EXPECT_EQ(0, os::system(" "));
}


TEST_F(OsTest, nonblock)
{
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));

  Try<bool> isNonBlock = false;

  isNonBlock = os::isNonblock(pipes[0]);
  ASSERT_SOME(isNonBlock);
  EXPECT_FALSE(isNonBlock.get());

  ASSERT_SOME(os::nonblock(pipes[0]));

  isNonBlock = os::isNonblock(pipes[0]);
  ASSERT_SOME(isNonBlock);
  EXPECT_TRUE(isNonBlock.get());

  close(pipes[0]);
  close(pipes[1]);

  EXPECT_ERROR(os::nonblock(pipes[0]));
  EXPECT_ERROR(os::nonblock(pipes[0]));
}


TEST_F(OsTest, touch)
{
  const string& testfile  = path::join(os::getcwd(), UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}


TEST_F(OsTest, readWriteString)
{
  const string& testfile  = path::join(os::getcwd(), UUID::random().toString());
  const string& teststr = "line1\nline2";

  ASSERT_SOME(os::write(testfile, teststr));

  Try<string> readstr = os::read(testfile);

  ASSERT_SOME(readstr);
  EXPECT_EQ(teststr, readstr.get());
}


TEST_F(OsTest, find)
{
  const string& testdir = path::join(os::getcwd(), UUID::random().toString());
  const string& subdir = testdir + "/test1";
  ASSERT_SOME(os::mkdir(subdir)); // Create the directories.

  // Now write some files.
  const string& file1 = testdir + "/file1.txt";
  const string& file2 = subdir + "/file2.txt";
  const string& file3 = subdir + "/file3.jpg";

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  // Find "*.txt" files.
  Try<std::list<string> > result = os::find(testdir, ".txt");
  ASSERT_SOME(result);

  hashset<string> files;
  foreach (const string& file, result.get()) {
    files.insert(file);
  }

  ASSERT_EQ(2u, files.size());
  ASSERT_TRUE(files.contains(file1));
  ASSERT_TRUE(files.contains(file2));
}


TEST_F(OsTest, bootId)
{
  Try<string> bootId = os::bootId();
  ASSERT_SOME(bootId);
  EXPECT_NE("", bootId.get());

#ifdef __linux__
  Try<string> read = os::read("/proc/sys/kernel/random/boot_id");
  ASSERT_SOME(read);
  EXPECT_EQ(bootId.get(), strings::trim(read.get()));
#elif defined(__APPLE__)
  // For OS X systems, the boot id is the system boot time in
  // seconds, so assert it can be numified and is a reasonable value.
  Try<uint64_t> numified = numify<uint64_t>(bootId.get());
  ASSERT_SOME(numified);

  timeval time;
  gettimeofday(&time, NULL);
  EXPECT_GT(Seconds(numified.get()), Seconds(0));
  EXPECT_LT(Seconds(numified.get()), Seconds(time.tv_sec));
#endif
}


TEST_F(OsTest, uname)
{
  const Try<os::UTSInfo>& info = os::uname();

  ASSERT_SOME(info);
#ifdef __linux__
  EXPECT_EQ(info.get().sysname, "Linux");
#endif
#ifdef __APPLE__
  EXPECT_EQ(info.get().sysname, "Darwin");
#endif
}


TEST_F(OsTest, sysname)
{
  const Try<string>& name = os::sysname();

  ASSERT_SOME(name);
#ifdef __linux__
  EXPECT_EQ(name.get(), "Linux");
#endif
#ifdef __APPLE__
  EXPECT_EQ(name.get(), "Darwin");
#endif
}


TEST_F(OsTest, release)
{
  const Try<os::Release>& info = os::release();

  ASSERT_SOME(info);
}


TEST_F(OsTest, sleep)
{
  Duration duration = Milliseconds(10);
  Stopwatch stopwatch;
  stopwatch.start();
  ASSERT_SOME(os::sleep(duration));
  ASSERT_LE(duration, stopwatch.elapsed());

  ASSERT_ERROR(os::sleep(Milliseconds(-10)));
}


#ifdef __APPLE__
TEST_F(OsTest, sysctl)
{
  // String test.
  Try<os::UTSInfo> uname = os::uname();

  ASSERT_SOME(uname);

  Try<string> release = os::sysctl(CTL_KERN, KERN_OSRELEASE).string();

  ASSERT_SOME(release);
  EXPECT_EQ(uname.get().release, release.get());

  Try<string> type = os::sysctl(CTL_KERN, KERN_OSTYPE).string();

  ASSERT_SOME(type);
  EXPECT_EQ(uname.get().sysname, type.get());

  // Integer test.
  Try<int> maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();

  ASSERT_SOME(maxproc);

  // Table test.
  Try<vector<kinfo_proc> > processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(maxproc.get());

  ASSERT_SOME(processes);

  std::set<pid_t> pids;

  foreach (const kinfo_proc& process, processes.get()) {
    pids.insert(process.kp_proc.p_pid);
  }

  EXPECT_EQ(1, pids.count(getpid()));

  // Timeval test.
  Try<timeval> bootTime = os::sysctl(CTL_KERN, KERN_BOOTTIME).time();
  ASSERT_SOME(bootTime);

  timeval time;
  gettimeofday(&time, NULL);

  EXPECT_GT(Seconds(bootTime.get().tv_sec), Seconds(0));
  EXPECT_LT(Seconds(bootTime.get().tv_sec), Seconds(time.tv_sec));
}
#endif // __APPLE__


TEST_F(OsTest, pids)
{
  Try<set<pid_t> > pids = os::pids();
  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));

  pids = os::pids(getpgid(0), None());
  EXPECT_SOME(pids);
  EXPECT_GE(pids.get().size(), 1u);
  EXPECT_EQ(1u, pids.get().count(getpid()));

  EXPECT_ERROR(os::pids(-1, None()));

  pids = os::pids(None(), getsid(0));
  EXPECT_SOME(pids);
  EXPECT_GE(pids.get().size(), 1u);
  EXPECT_EQ(1u, pids.get().count(getpid()));

  EXPECT_ERROR(os::pids(None(), -1));
}


TEST_F(OsTest, children)
{
  Try<set<pid_t> > children = os::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(0u, children.get().size());

  Try<ProcessTree> tree =
    Fork(None(),                   // Child.
         Fork(Exec("sleep 10")),   // Grandchild.
         Exec("sleep 10"))();

  ASSERT_SOME(tree);
  ASSERT_EQ(1u, tree.get().children.size());

  pid_t child = tree.get().process.pid;
  pid_t grandchild = tree.get().children.front().process.pid;

  // Ensure the non-recursive children does not include the
  // grandchild.
  children = os::children(getpid(), false);

  ASSERT_SOME(children);
  EXPECT_EQ(1u, children.get().size());
  EXPECT_EQ(1u, children.get().count(child));

  children = os::children(getpid());

  ASSERT_SOME(children);

  // Depending on whether or not the shell has fork/exec'ed in each
  // above 'Exec', we could have 2 or 4 children. That is, some shells
  // might simply for exec the command above (i.e., 'sleep 10') while
  // others might fork/exec the command, keeping around a 'sh -c'
  // process as well.
  EXPECT_LE(2u, children.get().size());
  EXPECT_GE(4u, children.get().size());

  EXPECT_EQ(1u, children.get().count(child));
  EXPECT_EQ(1u, children.get().count(grandchild));

  // Cleanup by killing the descendant processes.
  EXPECT_EQ(0, kill(grandchild, SIGKILL));
  EXPECT_EQ(0, kill(child, SIGKILL));

  // We have to reap the child for running the tests in repetition.
  ASSERT_EQ(child, waitpid(child, NULL, 0));
}


TEST_F(OsTest, process)
{
  const Result<Process>& process = os::process(getpid());

  ASSERT_SOME(process);
  EXPECT_EQ(getpid(), process.get().pid);
  EXPECT_EQ(getppid(), process.get().parent);
  ASSERT_SOME(process.get().session);
  EXPECT_EQ(getsid(getpid()), process.get().session.get());

  ASSERT_SOME(process.get().rss);
  EXPECT_GT(process.get().rss.get(), 0);

  // NOTE: On Linux /proc is a bit slow to update the CPU times,
  // hence we allow 0 in this test.
  ASSERT_SOME(process.get().utime);
  EXPECT_GE(process.get().utime.get(), Nanoseconds(0));
  ASSERT_SOME(process.get().stime);
  EXPECT_GE(process.get().stime.get(), Nanoseconds(0));

  EXPECT_FALSE(process.get().command.empty());
}


TEST_F(OsTest, processes)
{
  const Try<list<Process> >& processes = os::processes();

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
      EXPECT_EQ(getsid(getpid()), process.session.get());

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


void dosetsid(void)
{
  if (::setsid() == -1) {
    perror("Failed to setsid");
    abort();
  }
}


TEST_F(OsTest, killtree)
{
  Try<ProcessTree> tree =
    Fork(dosetsid,                         // Child.
         Fork(None(),                      // Grandchild.
              Fork(None(),                 // Great-grandchild.
                   Fork(dosetsid,          // Great-great-granchild.
                        Exec("sleep 10")),
                   Exec("sleep 10")),
              Exec("exit 0")),
         Exec("sleep 10"))();

  ASSERT_SOME(tree);

  // The process tree we instantiate initially looks like this:
  //
  //  -+- child sleep 10
  //   \-+- grandchild exit 0
  //     \-+- greatGrandchild sleep 10
  //       \--- greatGreatGrandchild sleep 10
  //
  // But becomes two process trees after the grandchild exits:
  //
  //  -+- child sleep 10
  //   \--- grandchild (exit 0)
  //
  //  -+- greatGrandchild sleep 10
  //   \--- greatGreatGrandchild sleep 10

  // Grab the pids from the instantiated process tree.
  ASSERT_EQ(1u, tree.get().children.size());
  ASSERT_EQ(1u, tree.get().children.front().children.size());
  ASSERT_EQ(1u, tree.get().children.front().children.front().children.size());

  pid_t child = tree.get();
  pid_t grandchild = tree.get().children.front();
  pid_t greatGrandchild = tree.get().children.front().children.front();
  pid_t greatGreatGrandchild =
    tree.get().children.front().children.front().children.front();

  // Now wait for the grandchild to exit splitting the process tree.
  Duration elapsed = Duration::zero();
  while (true) {
    Result<os::Process> process = os::process(grandchild);

    ASSERT_FALSE(process.isError());

    if (process.isNone() || process.get().zombie) {
      break;
    }

    if (elapsed > Seconds(10)) {
      FAIL() << "Granchild process '" << process.get().pid << "' "
             << "(" << process.get().command << ") did not terminate";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }

  // Kill the process tree and follow sessions and groups to make sure
  // we cross the broken link due to the grandchild.
  Try<std::list<ProcessTree> > trees =
    os::killtree(child, SIGKILL, true, true);

  ASSERT_SOME(trees);

  EXPECT_EQ(2u, trees.get().size()) << stringify(trees.get());

  foreach (const ProcessTree& tree, trees.get()) {
    if (tree.process.pid == child) {
      // The 'grandchild' _might_ still be in the tree, just zombied,
      // unless the 'child' reaps the 'grandchild', which may happen
      // if the shell "sticks around" (i.e., some invocations of 'sh
      // -c' will 'exec' the command which will likely not do any
      // reaping, but in other cases an invocation of 'sh -c' will not
      // 'exec' the command, for example when the command is a
      // sequence of commands separated by ';').
      EXPECT_FALSE(tree.contains(greatGrandchild)) << tree;
      EXPECT_FALSE(tree.contains(greatGreatGrandchild)) << tree;
    } else if (tree.process.pid == greatGrandchild) {
      EXPECT_TRUE(tree.contains(greatGreatGrandchild)) << tree;
    } else {
      FAIL()
        << "Not expecting a process tree rooted at "
        << tree.process.pid << "\n" << tree;
    }
  }

  // All processes should be reaped since we've killed everything.
  // The direct child must be reaped by us below.
  elapsed = Duration::zero();
  while (true) {
    Result<os::Process> _child = os::process(child);
    ASSERT_SOME(_child);

    if (os::process(greatGreatGrandchild).isNone() &&
        os::process(greatGrandchild).isNone() &&
        os::process(grandchild).isNone() &&
        _child.get().zombie) {
      break;
    }

    if (elapsed > Seconds(10)) {
      FAIL() << "Processes were not reaped after killtree invocation";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }

  // Expect the pids to be wiped!
  EXPECT_NONE(os::process(greatGreatGrandchild));
  EXPECT_NONE(os::process(greatGrandchild));
  EXPECT_NONE(os::process(grandchild));
  EXPECT_SOME(os::process(child));
  EXPECT_TRUE(os::process(child).get().zombie);

  // We have to reap the child for running the tests in repetition.
  ASSERT_EQ(child, waitpid(child, NULL, 0));
}


TEST_F(OsTest, pstree)
{
  Try<ProcessTree> tree = os::pstree(getpid());

  ASSERT_SOME(tree);
  EXPECT_EQ(0u, tree.get().children.size()) << stringify(tree.get());

  tree =
    Fork(None(),                   // Child.
         Fork(Exec("sleep 10")),   // Grandchild.
         Exec("sleep 10"))();

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
  ASSERT_EQ(child, waitpid(child, NULL, 0));
}


TEST_F(OsTest, ProcessExists)
{
  // Check we exist.
  EXPECT_TRUE(os::exists(::getpid()));

  // Check init/launchd/systemd exists.
  // NOTE: This should return true even if we don't have permission to signal
  // the pid.
  EXPECT_TRUE(os::exists(1));

  // Check existence of a child process through its lifecycle: running,
  // zombied, reaped.
  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // In child process.
    while (true) { sleep(1); }

    ABORT("Child should not reach this statement");
  }

  // In parent.
  EXPECT_TRUE(os::exists(pid));

  ASSERT_EQ(0, kill(pid, SIGKILL));

  // Wait until the process is a zombie.
  Duration elapsed = Duration::zero();
  while (true) {
    Result<os::Process> process = os::process(pid);
    ASSERT_SOME(process);

    if (process.get().zombie) {
      break;
    }

    ASSERT_LT(elapsed, Milliseconds(100));

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  };

  // The process should still 'exist', even if it's a zombie.
  EXPECT_TRUE(os::exists(pid));

  // Reap the zombie and confirm the process no longer exists.
  int status;

  EXPECT_EQ(pid, ::waitpid(pid, &status, 0));
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));

  EXPECT_FALSE(os::exists(pid));
}


TEST_F(OsTest, user)
{
  std::ostringstream user_;
  EXPECT_SOME_EQ(0, os::shell(&user_ , "id -un"));

  Result<string> user = os::user();
  ASSERT_SOME_EQ(strings::trim(user_.str()), user);

  std::ostringstream uid_;
  EXPECT_SOME_EQ(0, os::shell(&uid_, "id -u"));
  Try<uid_t> uid = numify<uid_t>(strings::trim(uid_.str()));
  ASSERT_SOME(uid);
  EXPECT_SOME_EQ(uid.get(), os::getuid(user.get()));

  std::ostringstream gid_;
  EXPECT_SOME_EQ(0, os::shell(&gid_, "id -g"));
  Try<gid_t> gid = numify<gid_t>(strings::trim(gid_.str()));
  ASSERT_SOME(gid);
  EXPECT_SOME_EQ(gid.get(), os::getgid(user.get()));

  EXPECT_NONE(os::getuid(UUID::random().toString()));
  EXPECT_NONE(os::getgid(UUID::random().toString()));

  EXPECT_TRUE(os::su(user.get()));
  EXPECT_FALSE(os::su(UUID::random().toString()));
}
