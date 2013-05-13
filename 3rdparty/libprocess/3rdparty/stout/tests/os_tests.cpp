#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <cstdlib> // For rand.
#include <list>
#include <set>
#include <string>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#ifdef __APPLE__
#include <stout/os/sysctl.hpp>
#endif

using std::list;
using std::set;
using std::string;


static hashset<string> listfiles(const string& directory)
{
  hashset<string> fileset;
  foreach (const string& file, os::ls(directory)) {
    fileset.insert(file);
  }
  return fileset;
}


class OsTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    Try<string> mkdtemp = os::mkdtemp();
    ASSERT_SOME(mkdtemp);
    tmpdir = mkdtemp.get();
  }

  virtual void TearDown()
  {
    ASSERT_SOME(os::rmdir(tmpdir));
  }

  string tmpdir;
};


TEST_F(OsTest, rmdir)
{
  const hashset<string> EMPTY;

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
  const string& testfile  = tmpdir + "/" + UUID::random().toString();

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}


TEST_F(OsTest, readWriteString)
{
  const string& testfile  = tmpdir + "/" + UUID::random().toString();
  const string& teststr = "test";

  ASSERT_SOME(os::write(testfile, teststr));

  Try<string> readstr = os::read(testfile);

  ASSERT_SOME(readstr);
  EXPECT_EQ(teststr, readstr.get());
}


TEST_F(OsTest, find)
{
  const string& testdir = tmpdir + "/" + UUID::random().toString();
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


TEST_F(OsTest, uname)
{
  Try<os::UTSInfo> info = os::uname();

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
  Try<string> name = os::sysname();

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
  Try<os::Release> info = os::release();

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
  Try<os::UTSInfo> uname = os::uname();

  ASSERT_SOME(uname);

  Try<string> release = os::sysctl(CTL_KERN, KERN_OSRELEASE).string();

  ASSERT_SOME(release);
  EXPECT_EQ(uname.get().release, release.get());

  Try<string> type = os::sysctl(CTL_KERN, KERN_OSTYPE).string();

  ASSERT_SOME(type);
  EXPECT_EQ(uname.get().sysname, type.get());

  Try<int> maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();

  ASSERT_SOME(maxproc);

  Try<std::vector<kinfo_proc> > processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(maxproc.get());

  ASSERT_SOME(processes);

  std::set<pid_t> pids;

  foreach (const kinfo_proc& process, processes.get()) {
    pids.insert(process.kp_proc.p_pid);
  }

  EXPECT_EQ(1, pids.count(getpid()));
}
#endif // __APPLE__


TEST_F(OsTest, pids)
{
  Try<set<pid_t> > pids = os::pids();

  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));
}


TEST_F(OsTest, children)
{
  Try<set<pid_t> > children = os::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(0u, children.get().size());

  // Use pipes to determine the pids of the child and grandchild.
  int childPipes[2];
  int grandchildPipes[2];
  ASSERT_NE(-1, pipe(childPipes));
  ASSERT_NE(-1, pipe(grandchildPipes));

  pid_t child;
  pid_t grandchild;
  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    close(childPipes[1]);
    close(grandchildPipes[1]);

    // Get the pids via the pipes.
    ASSERT_NE(-1, read(childPipes[0], &child, sizeof(child)));
    ASSERT_NE(-1, read(grandchildPipes[0], &grandchild, sizeof(grandchild)));

    close(childPipes[0]);
    close(grandchildPipes[0]);
  } else {
    // In child process.
    close(childPipes[0]);
    close(grandchildPipes[0]);

    // Double fork!
    if ((pid = fork()) == -1) {
      perror("Failed to fork a grand child process");
      abort();
    }

    if (pid > 0) {
      // Still in child process.
      pid = getpid();
      if (write(childPipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }

      close(childPipes[1]);

      while (true); // Keep waiting until we get a signal.
    } else {
      // In grandchild process.
      pid = getpid();
      if (write(grandchildPipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }

      close(grandchildPipes[1]);

      while (true); // Keep waiting until we get a signal.
    }
  }

  // Ensure the non-recursive children does not include the
  // grandchild.
  children = os::children(getpid(), false);

  ASSERT_SOME(children);
  EXPECT_EQ(1u, children.get().size());
  EXPECT_EQ(1u, children.get().count(child));

  children = os::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(2u, children.get().size());
  EXPECT_EQ(1u, children.get().count(child));
  EXPECT_EQ(1u, children.get().count(grandchild));

  // Cleanup by killing the descendant processes.
  EXPECT_EQ(0, kill(grandchild, SIGKILL)) << strerror(errno);
  EXPECT_EQ(0, kill(child, SIGKILL)) << strerror(errno);

  // We have to reap the child for running the tests in repetition.
  ASSERT_EQ(child, waitpid(child, NULL, 0)) << strerror(errno);
}


TEST_F(OsTest, process)
{
  Try<os::Process> status = os::process(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status.get().pid);
  EXPECT_EQ(getppid(), status.get().parent);
  EXPECT_EQ(getsid(getpid()), status.get().session);
  EXPECT_GT(status.get().rss, 0);

  // NOTE: On Linux /proc is a bit slow to update the CPU times,
  // hence we allow 0 in this test.
  EXPECT_GE(status.get().utime, Nanoseconds(0));
  EXPECT_GE(status.get().stime, Nanoseconds(0));

  EXPECT_FALSE(status.get().command.empty());
}


TEST_F(OsTest, processes)
{
  Try<list<os::Process> > processes = os::processes();

  ASSERT_SOME(processes);
  ASSERT_GT(processes.get().size(), 2);

  // Look for ourselves in the table.
  bool found = false;
  foreach (const os::Process& process, processes.get()) {
    if (process.pid == getpid()) {
      found = true;
      EXPECT_EQ(getpid(), process.pid);
      EXPECT_EQ(getppid(), process.parent);
      EXPECT_EQ(getsid(getpid()), process.session);
      EXPECT_GT(process.rss, 0);

      // NOTE: On linux /proc is a bit slow to update the cpu times,
      // hence we allow 0 in this test.
      EXPECT_GE(process.utime, Nanoseconds(0));
      EXPECT_GE(process.stime, Nanoseconds(0));

      EXPECT_FALSE(process.command.empty());

      break;
    }
  }

  EXPECT_TRUE(found);
}
