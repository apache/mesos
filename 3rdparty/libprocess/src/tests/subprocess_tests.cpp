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
#include <stout/path.hpp>

#include <stout/os/read.hpp>

#include <stout/tests/utils.hpp>

using namespace process;

using std::map;
using std::string;


class SubprocessTest: public TemporaryDirectoryTest {};


TEST_F(SubprocessTest, Status)
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
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

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
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(1, WEXITSTATUS(status));

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
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGTERM, WTERMSIG(status));

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
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));

  Clock::resume();
}


TEST_F(SubprocessTest, PipeOutput)
{
  Clock::pause();

  // Standard out.
  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Standard error.
  s = subprocess(
      "echo hello 1>&2",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().err());
  ASSERT_SOME(os::nonblock(s.get().err().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().err().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, PipeInput)
{
  Clock::pause();

  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().in());
  ASSERT_SOME(os::write(s.get().in().get(), "hello\n"));

  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, PipeSplice)
{
  Clock::pause();

  Try<Subprocess> s = subprocess(
      "echo 'hello world'",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);

  // Create a temporary file for splicing into.
  string path = path::join(os::getcwd(), "stdout");

  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_SOME(fd);
  ASSERT_SOME(os::nonblock(fd.get()));

  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_READY(io::splice(s.get().out().get(), fd.get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Now make sure all the data is there!
  Try<string> read = os::read(path);
  ASSERT_SOME(read);
  EXPECT_EQ("hello world\n", read.get());

  Clock::resume();
}


TEST_F(SubprocessTest, PathOutput)
{
  Clock::pause();

  string out = path::join(os::getcwd(), "stdout");
  string err = path::join(os::getcwd(), "stderr");

  // Standard out.
  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::PIPE(),
      Subprocess::PATH(out),
      Subprocess::PIPE());

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Try<string> read = os::read(out);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  // Standard error.
  s = subprocess(
      "echo hello 1>&2",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PATH(err));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  read = os::read(err);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  Clock::resume();
}


TEST_F(SubprocessTest, PathInput)
{
  Clock::pause();

  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::PATH(in),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, FdOutput)
{
  Clock::pause();

  string out = path::join(os::getcwd(), "stdout");
  string err = path::join(os::getcwd(), "stderr");

  // Standard out.
  Try<int> outFd = os::open(
      out,
      O_WRONLY | O_CREAT | O_APPEND,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_SOME(outFd);

  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::PIPE(),
      Subprocess::FD(outFd.get()),
      Subprocess::PIPE());

  ASSERT_SOME(os::close(outFd.get()));
  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Try<string> read = os::read(out);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  // Standard error.
  Try<int> errFd = os::open(
      err,
      O_WRONLY | O_CREAT | O_APPEND,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_SOME(errFd);

  s = subprocess(
      "echo hello 1>&2",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::FD(errFd.get()));

  ASSERT_SOME(os::close(errFd.get()));
  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  read = os::read(err);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  Clock::resume();
}


TEST_F(SubprocessTest, FdInput)
{
  Clock::pause();

  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<int> inFd = os::open(in, O_RDONLY, 0);
  ASSERT_SOME(inFd);

  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::FD(inFd.get()),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(os::close(inFd.get()));

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, Default)
{
  Clock::pause();

  Try<Subprocess> s = subprocess("echo hello world");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, Environment)
{
  Clock::pause();

  // Simple value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Multiple key-value pairs.
  environment.clear();
  environment["MESSAGE0"] = "hello";
  environment["MESSAGE1"] = "world";

  s = subprocess(
      "echo $MESSAGE0 $MESSAGE1",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, EnvironmentWithSpaces)
{
  Clock::pause();

  // Spaces in value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello world";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, EnvironmentWithSpacesAndQuotes)
{
  Clock::pause();

  // Spaces and quotes in value.
  map<string, string> environment;
  environment["MESSAGE"] = "\"hello world\"";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("\"hello world\"\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


TEST_F(SubprocessTest, EnvironmentOverride)
{
  Clock::pause();

  // Ensure we override an existing environment variable.
  os::setenv("MESSAGE", "hello");

  map<string, string> environment;
  environment["MESSAGE"] = "goodbye";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_EXPECT_EQ("goodbye\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  Clock::resume();
}


static int setupChdir(const string& directory)
{
  // Keep everything async-signal safe.
  if (::chdir(directory.c_str()) == -1) {
    return errno;
  }

  return 0;
}


TEST_F(SubprocessTest, Setup)
{
  Clock::pause();

  Try<string> directory = os::mkdtemp();
  ASSERT_SOME(directory);

  // chdir().
  Try<Subprocess> s = subprocess(
      "echo hello world > file",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
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


static int setupStatus(int ret)
{
  return ret;
}


TEST_F(SubprocessTest, SetupStatus)
{
  Clock::pause();

  // Exit 0 && setup 1.
  Try<Subprocess> s = subprocess(
      "exit 0",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
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
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
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
