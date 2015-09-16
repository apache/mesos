/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <signal.h>

#include <gmock/gmock.h>

#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
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
using std::vector;


class SubprocessTest: public TemporaryDirectoryTest {};


TEST_F(SubprocessTest, Status)
{
  // Exit 0.
  Try<Subprocess> s = subprocess("exit 0");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Exit 1.
  s = subprocess("exit 1");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}


TEST_F(SubprocessTest, PipeOutput)
{
  // Standard out.
  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().err().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, PipeInput)
{
  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().in());
  ASSERT_SOME(os::write(s.get().in().get(), "hello\n"));

  ASSERT_SOME(s.get().out());
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, PipeRedirect)
{
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
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);
  ASSERT_SOME(os::nonblock(fd.get()));

  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_READY(io::redirect(s.get().out().get(), fd.get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Now make sure all the data is there!
  Try<string> read = os::read(path);
  ASSERT_SOME(read);
  EXPECT_EQ("hello world\n", read.get());
}


TEST_F(SubprocessTest, PathOutput)
{
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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  read = os::read(err);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());
}


TEST_F(SubprocessTest, PathInput)
{
  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::PATH(in),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, FdOutput)
{
  string out = path::join(os::getcwd(), "stdout");
  string err = path::join(os::getcwd(), "stderr");

  // Standard out.
  Try<int> outFd = os::open(
      out,
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(outFd);

  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::PIPE(),
      Subprocess::FD(outFd.get()),
      Subprocess::PIPE());

  ASSERT_SOME(os::close(outFd.get()));
  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(errFd);

  s = subprocess(
      "echo hello 1>&2",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::FD(errFd.get()));

  ASSERT_SOME(os::close(errFd.get()));
  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  read = os::read(err);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());
}


TEST_F(SubprocessTest, FdInput)
{
  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<int> inFd = os::open(in, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(inFd);

  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::FD(inFd.get()),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  ASSERT_SOME(os::close(inFd.get()));

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, Default)
{
  Try<Subprocess> s = subprocess("echo hello world");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


struct Flags : public flags::FlagsBase
{
  Flags()
  {
    add(&b, "b", "bool");
    add(&i, "i", "int");
    add(&s, "s", "string");
    add(&s2, "s2", "string with single quote");
    add(&s3, "s3", "string with double quote");
    add(&d, "d", "Duration");
    add(&y, "y", "Bytes");
    add(&j, "j", "JSON::Object");
  }

  Option<bool> b;
  Option<int> i;
  Option<string> s;
  Option<string> s2;
  Option<string> s3;
  Option<Duration> d;
  Option<Bytes> y;
  Option<JSON::Object> j;
};


TEST_F(SubprocessTest, Flags)
{
  Flags flags;
  flags.b = true;
  flags.i = 42;
  flags.s = "hello";
  flags.s2 = "we're";
  flags.s3 = "\"geek\"";
  flags.d = Seconds(10);
  flags.y = Bytes(100);

  JSON::Object object;
  object.values["strings"] = "string";
  object.values["integer1"] = 1;
  object.values["integer2"] = -1;
  object.values["double1"] = 1;
  object.values["double2"] = -1;
  object.values["double3"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";
  object.values["nested"] = nested;

  JSON::Array array;
  array.values.push_back(nested);
  object.values["array"] = array;

  flags.j = object;

  string out = path::join(os::getcwd(), "stdout");

  Try<Subprocess> s = subprocess(
      "/bin/echo",
      vector<string>(1, "echo"),
      Subprocess::PIPE(),
      Subprocess::PATH(out),
      Subprocess::PIPE(),
      flags);

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));

  // Parse the output and make sure that it matches the flags we
  // specified in the beginning.
  Try<string> read = os::read(out);
  ASSERT_SOME(read);

  // TODO(jieyu): Consider testing the case where escaped spaces exist
  // in the arguments.
  vector<string> split = strings::split(read.get(), " ");
  int argc = split.size() + 1;
  char** argv = new char*[argc];
  argv[0] = (char*) "command";
  for (int i = 1; i < argc; i++) {
    argv[i] = ::strdup(split[i - 1].c_str());
  }

  Flags flags2;
  Try<Nothing> load = flags2.load(None(), argc, argv);
  ASSERT_SOME(load);
  EXPECT_EQ(flags.b, flags2.b);
  EXPECT_EQ(flags.i, flags2.i);
  EXPECT_EQ(flags.s, flags2.s);
  EXPECT_EQ(flags.s2, flags2.s2);
  EXPECT_EQ(flags.s3, flags2.s3);
  EXPECT_EQ(flags.d, flags2.d);
  EXPECT_EQ(flags.y, flags2.y);
  EXPECT_EQ(flags.j, flags2.j);

  for (int i = 1; i < argc; i++) {
    ::free(argv[i]);
  }
  delete[] argv;
}


TEST_F(SubprocessTest, Environment)
{
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
  AWAIT_EXPECT_EQ("hello\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, EnvironmentWithSpaces)
{
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
  AWAIT_EXPECT_EQ("hello world\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, EnvironmentWithSpacesAndQuotes)
{
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
  AWAIT_EXPECT_EQ("\"hello world\"\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}


TEST_F(SubprocessTest, EnvironmentOverride)
{
  // Ensure we override an existing environment variable.
  os::setenv("MESSAGE1", "hello");
  os::setenv("MESSAGE2", "world");

  map<string, string> environment;
  environment["MESSAGE2"] = "goodbye";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE1 $MESSAGE2",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s.get().out());
  AWAIT_EXPECT_EQ("goodbye\n", io::read(s.get().out().get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  // Make sure 'file' is there and contains 'hello world'.
  const string path = path::join(directory.get(), "file");
  EXPECT_TRUE(os::exists(path));
  EXPECT_SOME_EQ("hello world\n", os::read(path));

  os::rmdir(directory.get());
}


static int setupStatus(int ret)
{
  return ret;
}


TEST_F(SubprocessTest, SetupStatus)
{
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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

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
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  status = s.get().status().get().get();

  // Verify we received the command status.
  ASSERT_EQ(1, WEXITSTATUS(status));
}
