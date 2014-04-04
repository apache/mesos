#include <signal.h>

#include <gmock/gmock.h>

#include <sys/types.h>

#include <map>
#include <string>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/list.hpp>
#include <stout/os/read.hpp>
#include <stout/path.hpp>

using namespace process;

using std::map;
using std::string;


TEST(Subprocess, status)
{
  Clock::pause();

  // Exit 0.
  Try<Subprocess> s = subprocess("exit 0");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Exit 1.
  s = subprocess("exit 1");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(1, WEXITSTATUS(status));

  // SIGTERM.
  s = subprocess("sleep 60");

  ASSERT_SOME(s);

  kill(s.get().pid(), SIGTERM);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  ASSERT_TRUE(WIFSIGNALED(status));
  ASSERT_EQ(SIGTERM, WTERMSIG(status));

  // SIGKILL.
  s = subprocess("sleep 60");

  ASSERT_SOME(s);

  kill(s.get().pid(), SIGKILL);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  ASSERT_TRUE(WIFSIGNALED(status));
  ASSERT_EQ(SIGKILL, WTERMSIG(status));

  Clock::resume();
}


TEST(Subprocess, output)
{
  Clock::pause();

  // Standard out.
  Try<Subprocess> s = subprocess("echo hello");

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Standard error.
  s = subprocess("echo hello 1>&2");

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().err()));

  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().err()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST(Subprocess, input)
{
  Clock::pause();

  Try<Subprocess> s = subprocess("read word ; echo $word");

  ASSERT_SOME(s);

  ASSERT_SOME(os::write(s.get().in(), "hello\n"));

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST(Subprocess, splice)
{
  Clock::pause();

  Try<Subprocess> s = subprocess("echo 'hello world'");

  ASSERT_SOME(s);

  // Create a temporary file for splicing into.
  Try<string> path = os::mktemp();
  ASSERT_SOME(path);

  Try<int> fd = os::open(
      path.get(),
      O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);
  ASSERT_SOME(fd);
  ASSERT_SOME(os::nonblock(fd.get()));

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_READY(io::splice(s.get().out(), fd.get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Now make sure all the data is there!
  Try<string> read = os::read(path.get());
  ASSERT_SOME(read);
  EXPECT_EQ("hello world\n", read.get());

  Clock::resume();
}


TEST(Subprocess, environment)
{
  Clock::pause();

  // Simple value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello";
  Try<Subprocess> s = subprocess("echo $MESSAGE", environment);

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Multiple key-value pairs.
  environment.clear();
  environment["MESSAGE0"] = "hello";
  environment["MESSAGE1"] = "world";
  s = subprocess("echo $MESSAGE0 $MESSAGE1", environment);

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));
}


TEST(Subprocess, environmentWithSpaces)
{
  Clock::pause();

  // Spaces in value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello world";
  Try<Subprocess> s = subprocess("echo $MESSAGE", environment);

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));
}


TEST(Subprocess, environmentWithSpacesAndQuotes)
{
  Clock::pause();

  // Spaces and quotes in value.
  map<string, string> environment;
  environment["MESSAGE"] = "\"hello world\"";
  Try<Subprocess> s = subprocess("echo $MESSAGE", environment);

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("\"hello world\"\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));
}


TEST(Subprocess, environmentOverride)
{
  Clock::pause();

  // Ensure we override an existing environment variable.
  os::setenv("MESSAGE", "hello");

  map<string, string> environment;
  environment["MESSAGE"] = "goodbye";
  Try<Subprocess> s = subprocess("echo $MESSAGE", environment);

  ASSERT_SOME(s);

  ASSERT_SOME(os::nonblock(s.get().out()));

  AWAIT_EXPECT_EQ("goodbye\n", io::read(s.get().out()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());
  int status = s.get().status().get().get();

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));
}


int setupChdir(const string& directory)
{
  // Keep everything async-signal safe.
  if (::chdir(directory.c_str()) == -1) {
    return errno;
  }

  return 0;
}


TEST(Subprocess, setup)
{
  Clock::pause();

  Try<string> directory = os::mkdtemp();
  ASSERT_SOME(directory);

  // chdir().
  Try<Subprocess> s = subprocess(
      "echo hello world > file",
      None(),
      lambda::bind(&setupChdir, directory.get()));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  // Make sure 'file' is there and contains 'hello world'.
  const string& path = path::join(directory.get(), "file");
  EXPECT_TRUE(os::exists(path));
  EXPECT_SOME_EQ("hello world\n", os::read(path));

  os::rmdir(directory.get());

  Clock::resume();
}


int setupStatus(int ret)
{
  return ret;
}


TEST(Subprocess, setupStatus)
{
  Clock::pause();

  // Exit 0 && setup 1.
  Try<Subprocess> s = subprocess(
      "exit 0",
      None(),
      lambda::bind(&setupStatus, 1));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();

  // Verify we received the setup returned value instead of the
  // command status.
  ASSERT_EQ(1, WEXITSTATUS(status));

  // Exit 1 && setup 0.
  s = subprocess(
      "exit 1",
      None(),
      lambda::bind(&setupStatus, 0));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();

  // Verify we received the command status.
  ASSERT_EQ(1, WEXITSTATUS(status));

  Clock::resume();
}
