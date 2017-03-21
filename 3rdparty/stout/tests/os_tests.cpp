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

#include <stdint.h>

#if !defined(__linux__) && !defined(__WINDOWS__)
#include <sys/time.h> // For gettimeofday.
#endif
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include <cstdlib> // For rand.
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/os/environment.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/kill.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/write.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stout/os/sysctl.hpp>
#endif

#include <stout/tests/utils.hpp>

#ifndef __WINDOWS__
using os::Exec;
#endif // __WINDOWS__
using os::Fork;
using os::Process;
using os::ProcessTree;

using std::list;
using std::set;
using std::string;
using std::vector;


class OsTest : public TemporaryDirectoryTest {};


#ifndef __WINDOWS__
TEST_F(OsTest, Environment)
{
  // Make sure the environment has some entries with '=' in the value.
  os::setenv("SOME_SPECIAL_FLAG", "--flag=foobar");

  char** environ = os::raw::environment();

  hashmap<string, string> environment = os::environment();

  for (size_t index = 0; environ[index] != nullptr; index++) {
    string entry(environ[index]);
    size_t position = entry.find_first_of('=');
    if (position == string::npos) {
      continue; // Skip malformed environment entries.
    }
    const string key = entry.substr(0, position);
    const string value = entry.substr(position + 1);
    EXPECT_TRUE(environment.contains(key));
    EXPECT_EQ(value, environment[key]);
  }
}
#endif // __WINDOWS__


TEST_F(OsTest, TrivialEnvironment)
{
  // NOTE: Regression test that ensures Windows can get and set an environment
  // variable. This is easy to break: Windows maintains two non-compatible ways
  // to get and set environment variables: the CRT way (using `environ`,
  // `setenv`, and `getenv`), and the Win32 way (using `GetEnvironmentStrings`,
  // `SetEnvironmentVariable`, and `GetEnvironmentVariable`). This test makes
  // sure that we consistently back the Stout environment variable APIs with
  // with one or the other; a mix won't work.
  const string key = "test_key1";
  const string value = "value";
  os::setenv(key, value);

  hashmap<string, string> environment = os::environment();
  ASSERT_TRUE(environment.count(key) != 0);
  ASSERT_EQ(value, environment[key]);

  // NOTE: Regression test that ensures we can set an environment variable to
  // be an empty string. On Windows, this is only possible with the Win32 APIs:
  // the CRT `environ` macro will simply delete the variable if it is passed an
  // empty string as a value.
  os::setenv(key, "", true);

  environment = os::environment();
  ASSERT_TRUE(environment.count(key) != 0);
  ASSERT_EQ("", environment[key]);
}


TEST_F(OsTest, Argv)
{
  vector<string> args = {"arg0", "arg1", "arg2"};

  os::raw::Argv _argv(args);
  char** argv = _argv;
  for (size_t i = 0; i < args.size(); i++) {
    EXPECT_EQ(args[i], argv[i]);
  }
}


TEST_F(OsTest, System)
{
  EXPECT_EQ(0, os::system("exit 0"));
  EXPECT_EQ(0, os::system(SLEEP_COMMAND(0)));
  EXPECT_NE(0, os::system("exit 1"));
  EXPECT_NE(0, os::system("invalid.command"));

  // Note that ::system returns 0 for the following two cases as well.
  EXPECT_EQ(0, os::system(""));
  EXPECT_EQ(0, os::system(" "));
}


// NOTE: Disabled because `os::cloexec` is not implemented on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Cloexec)
{
  Try<int_fd> fd = os::open(
      "cloexec",
      O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);
  EXPECT_SOME_TRUE(os::isCloexec(fd.get()));

  ASSERT_SOME(os::unsetCloexec(fd.get()));
  EXPECT_SOME_FALSE(os::isCloexec(fd.get()));

  close(fd.get());

  fd = os::open(
      "non-cloexec",
      O_CREAT | O_WRONLY | O_APPEND,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);
  EXPECT_SOME_FALSE(os::isCloexec(fd.get()));

  ASSERT_SOME(os::cloexec(fd.get()));
  EXPECT_SOME_TRUE(os::isCloexec(fd.get()));

  close(fd.get());
}


// NOTE: Disabled because `os::nonblock` doesn't exist on Windows.
#ifndef __WINDOWS__
TEST_F(OsTest, Nonblock)
{
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));

  Try<bool> isNonBlock = os::isNonblock(pipes[0]);
  EXPECT_SOME_FALSE(isNonBlock);

  ASSERT_SOME(os::nonblock(pipes[0]));

  isNonBlock = os::isNonblock(pipes[0]);
  EXPECT_SOME_TRUE(isNonBlock);

  close(pipes[0]);
  close(pipes[1]);

  EXPECT_ERROR(os::nonblock(pipes[0]));
  EXPECT_ERROR(os::nonblock(pipes[1]));
}
#endif // __WINDOWS__


// TODO(hausdorff): Fix this issue and enable the test on Windows. It fails
// because `os::size` is not following symlinks correctly on Windows. See
// MESOS-5939.
// Tests whether a file's size is reported by os::stat::size as expected.
// Tests all four combinations of following a link or not and of a file
// or a link as argument. Also tests that an error is returned for a
// non-existing file.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Size)
{
  const string file = path::join(os::getcwd(), UUID::random().toString());

  const Bytes size = 1053;

  ASSERT_SOME(os::write(file, string(size.bytes(), 'X')));

  // The reported file size should be the same whether following links
  // or not, given that the input parameter is not a link.
  EXPECT_SOME_EQ(size, os::stat::size(file, os::stat::FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(size, os::stat::size(file, os::stat::DO_NOT_FOLLOW_SYMLINK));

  EXPECT_ERROR(os::stat::size("aFileThatDoesNotExist"));

  const string link = path::join(os::getcwd(), UUID::random().toString());

  ASSERT_SOME(fs::symlink(file, link));

  // Following links we expect the file's size, not the link's.
  EXPECT_SOME_EQ(size, os::stat::size(link, os::stat::FOLLOW_SYMLINK));

  // Not following links, we expect the string length of the linked path.
  EXPECT_SOME_EQ(Bytes(file.size()),
                 os::stat::size(link, os::stat::DO_NOT_FOLLOW_SYMLINK));
}


TEST_F(OsTest, BootId)
{
  Try<string> bootId = os::bootId();
  ASSERT_SOME(bootId);
  EXPECT_NE("", bootId.get());

#ifdef __linux__
  Try<string> read = os::read("/proc/sys/kernel/random/boot_id");
  ASSERT_SOME(read);
  EXPECT_EQ(bootId.get(), strings::trim(read.get()));
#elif defined(__APPLE__) || defined(__FreeBSD__)
  // For OS X and FreeBSD systems, the boot id is the system boot time in
  // seconds, so assert it can be numified and is a reasonable value.
  Try<uint64_t> numified = numify<uint64_t>(bootId.get());
  ASSERT_SOME(numified);

  timeval time;
  gettimeofday(&time, nullptr);
  EXPECT_GT(Seconds(numified.get()), Seconds(0));
  EXPECT_LT(Seconds(numified.get()), Seconds(time.tv_sec));
#endif
}


// TODO(hausdorff): Enable test on Windows after we fix. The test hangs. See
// MESOS-3441.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Sleep)
{
  Duration duration = Milliseconds(10);
  Stopwatch stopwatch;
  stopwatch.start();
  ASSERT_SOME(os::sleep(duration));
  ASSERT_LE(duration, stopwatch.elapsed());

  ASSERT_ERROR(os::sleep(Milliseconds(-10)));
}


#if defined(__APPLE__) || defined(__FreeBSD__)
TEST_F(OsTest, Sysctl)
{
  // String test.
  Try<os::UTSInfo> uname = os::uname();

  ASSERT_SOME(uname);

  Try<string> release = os::sysctl(CTL_KERN, KERN_OSRELEASE).string();

  EXPECT_SOME_EQ(uname.get().release, release);

  Try<string> type = os::sysctl(CTL_KERN, KERN_OSTYPE).string();

  EXPECT_SOME_EQ(uname.get().sysname, type);

  // Integer test.
  Try<int> maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();

  ASSERT_SOME(maxproc);

  // Table test.
  Try<vector<kinfo_proc>> processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(maxproc.get());

  ASSERT_SOME(processes);

  std::set<pid_t> pids;

  foreach (const kinfo_proc& process, processes.get()) {
#ifdef __APPLE__
    pids.insert(process.kp_proc.p_pid);
#else
    pids.insert(process.ki_pid);
#endif // __APPLE__
  }

  EXPECT_EQ(1u, pids.count(getpid()));

  // Timeval test.
  Try<timeval> bootTime = os::sysctl(CTL_KERN, KERN_BOOTTIME).time();
  ASSERT_SOME(bootTime);

  timeval time;
  gettimeofday(&time, nullptr);

  EXPECT_GT(Seconds(bootTime.get().tv_sec), Seconds(0));
  EXPECT_LT(Seconds(bootTime.get().tv_sec), Seconds(time.tv_sec));
}
#endif // __APPLE__ || __FreeBSD__


// TODO(hausdorff): Enable when we implement `Fork` and `Exec`. See MESOS-3638.
#ifndef __WINDOWS__
TEST_F(OsTest, Children)
{
  Try<set<pid_t>> children = os::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(0u, children.get().size());

  Try<ProcessTree> tree =
    Fork(None(),                   // Child.
         Fork(Exec(SLEEP_COMMAND(10))),   // Grandchild.
         Exec(SLEEP_COMMAND(10)))();

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
  ASSERT_EQ(child, waitpid(child, nullptr, 0));
}


void dosetsid()
{
  if (::setsid() == -1) {
    ABORT(string("Failed to setsid: ") + os::strerror(errno));
  }
}


TEST_F(OsTest, Killtree)
{
  Try<ProcessTree> tree =
    Fork(&dosetsid,                        // Child.
         Fork(None(),                      // Grandchild.
              Fork(None(),                 // Great-grandchild.
                   Fork(&dosetsid,         // Great-great-granchild.
                        Exec(SLEEP_COMMAND(10))),
                   Exec(SLEEP_COMMAND(10))),
              Exec("exit 0")),
         Exec(SLEEP_COMMAND(10)))();

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
  Try<list<ProcessTree>> trees =
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
  ASSERT_EQ(child, waitpid(child, nullptr, 0));
}


TEST_F(OsTest, KilltreeNoRoot)
{
  Try<ProcessTree> tree =
    Fork(&dosetsid,       // Child.
         Fork(None(),     // Grandchild.
              Fork(None(),
                   Exec(SLEEP_COMMAND(100))),
              Exec(SLEEP_COMMAND(100))),
         Exec("exit 0"))();
  ASSERT_SOME(tree);

  // The process tree we instantiate initially looks like this:
  //
  // -+- child exit 0             [new session and process group leader]
  //  \-+- grandchild sleep 100
  //   \-+- great grandchild sleep 100
  //
  // But becomes the following tree after the child exits:
  //
  // -+- child (exited 0)
  //  \-+- grandchild sleep 100
  //   \-+- great grandchild sleep 100
  //
  // And gets reparented when we reap the child:
  //
  // -+- new parent
  //  \-+- grandchild sleep 100
  //   \-+- great grandchild sleep 100

  // Grab the pids from the instantiated process tree.
  ASSERT_EQ(1u, tree.get().children.size());
  ASSERT_EQ(1u, tree.get().children.front().children.size());

  pid_t child = tree.get();
  pid_t grandchild = tree.get().children.front();
  pid_t greatGrandchild = tree.get().children.front().children.front();

  // Wait for the child to exit.
  Duration elapsed = Duration::zero();
  while (true) {
    Result<os::Process> process = os::process(child);
    ASSERT_FALSE(process.isError());

    if (process.get().zombie) {
      break;
    }

    if (elapsed > Seconds(1)) {
      FAIL() << "Child process " << stringify(child) << " did not terminate";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }

  // Ensure we reap our child now.
  EXPECT_SOME(os::process(child));
  EXPECT_TRUE(os::process(child).get().zombie);
  ASSERT_EQ(child, waitpid(child, nullptr, 0));

  // Check the grandchild and great grandchild are still running.
  ASSERT_TRUE(os::exists(grandchild));
  ASSERT_TRUE(os::exists(greatGrandchild));

  // Check the subtree has been reparented: the parent is no longer
  // child (the root of the tree), and that the process is not a
  // zombie. This is done because some systems run a secondary init
  // process for the user (init --user) that does not have pid 1,
  // meaning we can't just check that the parent pid == 1.
  Result<os::Process> _grandchild = os::process(grandchild);
  ASSERT_SOME(_grandchild);
  ASSERT_NE(child, _grandchild.get().parent);
  ASSERT_FALSE(_grandchild.get().zombie);

  // Check to see if we're in a jail on FreeBSD in case we've been
  // reparented to pid 1
#if __FreeBSD__
  if (!isJailed()) {
#endif
  // Check that grandchild's parent is also not a zombie.
  Result<os::Process> currentParent = os::process(_grandchild.get().parent);
  ASSERT_SOME(currentParent);
  ASSERT_FALSE(currentParent.get().zombie);
#ifdef __FreeBSD__
  }
#endif


  // Kill the process tree. Even though the root process has exited,
  // we specify to follow sessions and groups which should kill the
  // grandchild and greatgrandchild.
  Try<list<ProcessTree>> trees = os::killtree(child, SIGKILL, true, true);

  ASSERT_SOME(trees);
  EXPECT_FALSE(trees.get().empty());

  // All processes should be reparented and reaped by init.
  elapsed = Duration::zero();
  while (true) {
    if (os::process(grandchild).isNone() &&
        os::process(greatGrandchild).isNone()) {
      break;
    }

    if (elapsed > Seconds(10)) {
      FAIL() << "Processes were not reaped after killtree invocation";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }

  EXPECT_NONE(os::process(grandchild));
  EXPECT_NONE(os::process(greatGrandchild));
}


TEST_F(OsTest, ProcessExists)
{
  // Check we exist.
  EXPECT_TRUE(os::exists(::getpid()));

  // In a FreeBSD jail, pid 1 may not exist.
#if !defined(__FreeBSD__)
  // Check init/launchd/systemd exists.
  // NOTE: This should return true even if we don't have permission to signal
  // the pid.
  EXPECT_TRUE(os::exists(1));
#endif

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
  EXPECT_WTERMSIG_EQ(SIGKILL, status);

  EXPECT_FALSE(os::exists(pid));
}


TEST_F(OsTest, User)
{
  Try<string> user_ = os::shell("id -un");
  EXPECT_SOME(user_);

  Result<string> user = os::user();
  ASSERT_SOME_EQ(strings::trim(user_.get()), user);

  Try<string> uid_ = os::shell("id -u");
  EXPECT_SOME(uid_);
  Try<uid_t> uid = numify<uid_t>(strings::trim(uid_.get()));
  ASSERT_SOME(uid);
  EXPECT_SOME_EQ(uid.get(), os::getuid(user.get()));

  Try<string> gid_ = os::shell("id -g");
  EXPECT_SOME(gid_);
  Try<gid_t> gid = numify<gid_t>(strings::trim(gid_.get()));
  ASSERT_SOME(gid);
  EXPECT_SOME_EQ(gid.get(), os::getgid(user.get()));

  // A random UUID is an invalid username on some platforms. Some
  // versions of Linux (e.g., RHEL7) treat invalid usernames
  // differently from valid-but-not-found usernames.
  EXPECT_NONE(os::getuid(UUID::random().toString()));
  EXPECT_NONE(os::getgid(UUID::random().toString()));

  // A username that is valid but that is unlikely to exist.
  EXPECT_NONE(os::getuid("zzzvaliduserzzz"));
  EXPECT_NONE(os::getgid("zzzvaliduserzzz"));

  EXPECT_SOME(os::su(user.get()));
  EXPECT_ERROR(os::su(UUID::random().toString()));

  Try<string> gids_ = os::shell("id -G " + user.get());
  EXPECT_SOME(gids_);

  Try<vector<string>> tokens =
    strings::split(strings::trim(gids_.get(), strings::ANY, "\n"), " ");

  ASSERT_SOME(tokens);
  std::sort(tokens.get().begin(), tokens.get().end());

  Try<vector<gid_t>> gids = os::getgrouplist(user.get());
  EXPECT_SOME(gids);

  vector<string> expected_gids;
  foreach (gid_t gid, gids.get()) {
    expected_gids.push_back(stringify(gid));
  }
  std::sort(expected_gids.begin(), expected_gids.end());

  EXPECT_EQ(tokens.get(), expected_gids);

  EXPECT_SOME(os::setgid(gid.get()));

  // 'setgroups' requires root permission.
  if (user.get() != "root") {
    return;
  }

  EXPECT_SOME(os::setgroups(gids.get(), uid.get()));
  EXPECT_SOME(os::setuid(uid.get()));
}


TEST_F(OsTest, Chown)
{
  using os::stat::DO_NOT_FOLLOW_SYMLINK;

  Result<uid_t> uid = os::getuid();
  ASSERT_SOME(uid);

  // 'chown' requires root permission.
  if (uid.get() != 0) {
    return;
  }

  // In the following tests, we chown to an artitrary UID. There is
  // no special significance to the value 9.

  // Set up a simple directory hierarchy.
  EXPECT_SOME(os::mkdir("chown/one/two/three"));
  EXPECT_SOME(os::touch("chown/one/file"));
  EXPECT_SOME(os::touch("chown/one/two/file"));
  EXPECT_SOME(os::touch("chown/one/two/three/file"));

  // Make a symlink back to the top of the tree so we can verify
  // that it isn't followed.
  EXPECT_SOME(fs::symlink("../../../../chown", "chown/one/two/three/link"));

  // Make a symlink to the middle of the tree and verify that chowning the
  // symlink does not chown that subtree.
  EXPECT_SOME(fs::symlink("chown/one/two", "two.link"));
  EXPECT_SOME(os::chown(9, 9, "two.link", true));
  EXPECT_SOME_EQ(9u, os::stat::uid("two.link", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(0u, os::stat::uid("chown", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(0u,
      os::stat::uid("chown/one/two/three/file", DO_NOT_FOLLOW_SYMLINK));

  // Recursively chown the whole tree.
  EXPECT_SOME(os::chown(9, 9, "chown", true));
  EXPECT_SOME_EQ(9u, os::stat::uid("chown", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(9u,
      os::stat::uid("chown/one/two/three/file", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(9u,
      os::stat::uid("chown/one/two/three/link", DO_NOT_FOLLOW_SYMLINK));

  // Chown the subtree with the embedded link back and verify that it
  // doesn't follow back to the top of the tree.
  EXPECT_SOME(os::chown(0, 0, "chown/one/two/three", true));
  EXPECT_SOME_EQ(9u, os::stat::uid("chown", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(0u,
      os::stat::uid("chown/one/two/three", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(0u,
      os::stat::uid("chown/one/two/three/link", DO_NOT_FOLLOW_SYMLINK));

  // Verify that non-recursive chown changes the directory and not
  // its contents.
  EXPECT_SOME(os::chown(0, 0, "chown/one", false));
  EXPECT_SOME_EQ(0u, os::stat::uid("chown/one", DO_NOT_FOLLOW_SYMLINK));
  EXPECT_SOME_EQ(9u, os::stat::uid("chown/one/file", DO_NOT_FOLLOW_SYMLINK));
}


TEST_F(OsTest, ChownNoAccess)
{
  Result<uid_t> uid = os::getuid();
  Result<gid_t> gid = os::getgid();

  ASSERT_SOME(uid);
  ASSERT_SOME(gid);

  // This test requires that we not be root, since root will
  // bypass access checks.
  if (uid.get() == 0) {
    return;
  }

  ASSERT_SOME(os::mkdir("one/two"));

  // Chown to ourself should be a noop.
  EXPECT_SOME(os::chown(uid.get(), gid.get(), "one/two", true));

  ASSERT_SOME(os::chmod("one/two", 0));

  // Recursive chown should now fail to fully recurse due to
  // the lack of permission on "one/two".
  EXPECT_ERROR(os::chown(uid.get(), gid.get(), "one", true));
  EXPECT_ERROR(os::chown(uid.get(), gid.get(), "one/two", true));

  // A non-recursive should succeed when the child is not traversable.
  EXPECT_SOME(os::chown(uid.get(), gid.get(), "one", false));

  // A non-recursive should succeed on the non-traversable child too.
  EXPECT_SOME(os::chown(uid.get(), gid.get(), "one/two", false));

  // Restore directory access so the rmdir can work.
  ASSERT_SOME(os::chmod("one/two", 0755));
}
#endif // __WINDOWS__


TEST_F(OsTest, TrivialUser)
{
  const Result<string> user1 = os::user();
  ASSERT_SOME(user1);
  ASSERT_NE("", user1.get());

#ifdef __WINDOWS__
  const Result<string> user2 = os::user(INT_MAX);
  ASSERT_ERROR(user2);
#endif // __WINDOWS__
}


// TODO(hausdorff): Look into enabling this on Windows. Right now,
// `LD_LIBRARY_PATH` doesn't exist on Windows, so `setPaths` doesn't work. See
// MESOS-5940.
// Test setting/resetting/appending to LD_LIBRARY_PATH environment
// variable (DYLD_LIBRARY_PATH on OS X).
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Libraries)
{
  const string path1 = "/tmp/path1";
  const string path2 = "/tmp/path1";
  string ldLibraryPath;
  const string originalLibraryPath = os::libraries::paths();

  // Test setPaths.
  os::libraries::setPaths(path1);
  EXPECT_EQ(os::libraries::paths(), path1);

  // Test appendPaths.
  // 1. With empty LD_LIBRARY_PATH.
  // 1a. Set LD_LIBRARY_PATH to an empty string.
  os::libraries::setPaths("");
  ldLibraryPath = os::libraries::paths();
  EXPECT_EQ(ldLibraryPath, "");

  // 1b. Now test appendPaths.
  os::libraries::appendPaths(path1);
  EXPECT_EQ(os::libraries::paths(), path1);

  // 2. With non-empty LD_LIBRARY_PATH.
  // 2a. Set LD_LIBRARY_PATH to some non-empty value.
  os::libraries::setPaths(path2);
  ldLibraryPath = os::libraries::paths();
  EXPECT_EQ(ldLibraryPath, path2);

  // 2b. Now test appendPaths.
  os::libraries::appendPaths(path1);
  EXPECT_EQ(os::libraries::paths(), path2 + ":" + path1);

  // Reset LD_LIBRARY_PATH.
  os::libraries::setPaths(originalLibraryPath);
  EXPECT_EQ(os::libraries::paths(), originalLibraryPath);
}


// TODO(hausdorff): Port this test to Windows; these shell commands as they
// exist now don't make much sense to the Windows cmd prompt. See MESOS-3441.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Shell)
{
  Try<string> result = os::shell("echo %s", "hello world");
  EXPECT_SOME_EQ("hello world\n", result);

  result = os::shell("foobar");
  EXPECT_ERROR(result);

  // The `|| true`` necessary so that os::shell() sees a success
  // exit code and returns stdout (which we have piped stderr to).
  result = os::shell("LC_ALL=C ls /tmp/foobar889076 2>&1 || true");
  ASSERT_SOME(result);
  EXPECT_TRUE(strings::contains(result.get(), "No such file or directory"));

  // Testing a more ambitious command that mutates the filesystem.
  const string path = "/tmp/os_tests.txt";
  result = os::shell("touch %s", path.c_str());
  EXPECT_SOME_EQ("", result);
  EXPECT_TRUE(os::exists(path));

  // Let's clean up, and ensure this worked too.
  result = os::shell("rm %s", path.c_str());
  EXPECT_SOME_EQ("", result);
  EXPECT_FALSE(os::exists("/tmp/os_tests.txt"));
}


// NOTE: Disabled on Windows because `mknod` does not exist.
#ifndef __WINDOWS__
TEST_F(OsTest, Mknod)
{
#ifdef __FreeBSD__
  // If we're in a jail on FreeBSD, we can't use mknod.
  if (isJailed()) {
      return;
  }
#endif

  // mknod requires root permission.
  Result<string> user = os::user();
  ASSERT_SOME(user);

  if (user.get() != "root") {
    return;
  }

  const string device = "null";

  const string existing = path::join("/dev", device);
  ASSERT_TRUE(os::exists(existing));

  Try<mode_t> mode = os::stat::mode(existing);
  ASSERT_SOME(mode);

  Try<dev_t> rdev = os::stat::rdev(existing);
  ASSERT_SOME(rdev);

  const string another = path::join(os::getcwd(), device);
  ASSERT_FALSE(os::exists(another));

  EXPECT_SOME(os::mknod(another, mode.get(), rdev.get()));

  EXPECT_SOME(os::rm(another));
}
#endif // __WINDOWS__


// TODO(hausdorff): Look into enabling this test on Windows. Currently it is
// not possible to create a symlink on Windows unless the target exists. See
// MESOS-5881.
TEST_F_TEMP_DISABLED_ON_WINDOWS(OsTest, Realpath)
{
  // Create a file.
  const Try<string> _testFile = os::mktemp();
  ASSERT_SOME(_testFile);
  ASSERT_SOME(os::touch(_testFile.get()));
  const string& testFile = _testFile.get();

  // Create a symlink pointing to a file.
  const string testLink = UUID::random().toString();
  ASSERT_SOME(fs::symlink(testFile, testLink));

  // Validate the symlink.
  const Try<ino_t> fileInode = os::stat::inode(testFile);
  ASSERT_SOME(fileInode);
  const Try<ino_t> linkInode = os::stat::inode(testLink);
  ASSERT_SOME(linkInode);
  ASSERT_EQ(fileInode.get(), linkInode.get());

  // Verify that the symlink resolves correctly.
  Result<string> resolved = os::realpath(testLink);
  ASSERT_SOME(resolved);
  EXPECT_TRUE(strings::contains(resolved.get(), testFile));

  // Verify that the file itself resolves correctly.
  resolved = os::realpath(testFile);
  ASSERT_SOME(resolved);
  EXPECT_TRUE(strings::contains(resolved.get(), testFile));

  // Remove the file and the symlink.
  os::rm(testFile);
  os::rm(testLink);
}


// NOTE: Disabled on Windows because `which` doesn't exist.
#ifndef __WINDOWS__
TEST_F(OsTest, Which)
{
  // TODO(jieyu): Test PATH search ordering and file execution bit.
  Option<string> which = os::which("ls");
  ASSERT_SOME(which);

  which = os::which("bar");
  EXPECT_NONE(which);
}
#endif // __WINDOWS__
