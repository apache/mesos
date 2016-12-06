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

#include <signal.h>

#include <gmock/gmock.h>

#include <sys/types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include <stout/os/close.hpp>
#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

namespace io = process::io;

using process::Clock;
using process::subprocess;
using process::Subprocess;
using process::MAX_REAP_INTERVAL;

using std::map;
using std::string;
using std::vector;
using std::shared_ptr;


class SubprocessTest: public TemporaryDirectoryTest {};


void run_subprocess(const lambda::function<Try<Subprocess>()>& createSubprocess)
{
  Try<Subprocess> s = createSubprocess();

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  // Check process exited cleanly.
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}


// Calls subprocess, pipes output to an open file descriptor (and specifically
// a file descriptor for a file, rather than a socket).
TEST_F(SubprocessTest, PipeOutputToFileDescriptor)
{
  Try<string> testdir = os::mkdtemp();
  ASSERT_SOME(testdir);

  // Create temporary files to pipe `stdin` to, and open it. We will pipe
  // output into this file.
  const string outfile_name = "out.txt";
  const string outfile = path::join(testdir.get(), outfile_name);
  ASSERT_SOME(os::touch(outfile));

  Try<int_fd> outfile_fd = os::open(outfile, O_RDWR);
  ASSERT_SOME(outfile_fd);

  // Create temporary files to pipe `stderr` to, and open it. We will pipe
  // error into this file.
  const string errorfile_name = "error.txt";
  const string errorfile = path::join(testdir.get(), errorfile_name);
  ASSERT_SOME(os::touch(errorfile));

  Try<int_fd> errorfile_fd = os::open(errorfile, O_RDWR);
  ASSERT_SOME(errorfile_fd);

  // RAII handle for the file descriptor increases chance that we clean up
  // after ourselves.
  const auto close_fd = [](int_fd* fd) { os::close(*fd); };

  shared_ptr<int_fd> safe_out_fd(&outfile_fd.get(), close_fd);
  shared_ptr<int_fd> safe_err_fd(&errorfile_fd.get(), close_fd);

  // Pipe simple string to output file.
  run_subprocess(
    [outfile_fd]() -> Try<Subprocess> {
      return subprocess(
          "echo hello",
          Subprocess::FD(STDIN_FILENO),
          Subprocess::FD(outfile_fd.get()),
          Subprocess::FD(STDERR_FILENO));
    });

  // Pipe simple string to error file.
  run_subprocess(
    [errorfile_fd]() -> Try<Subprocess> {
      return subprocess(
          "echo goodbye 1>&2",
          Subprocess::FD(STDIN_FILENO),
          Subprocess::FD(STDOUT_FILENO),
          Subprocess::FD(errorfile_fd.get()));
    });

  // Finally, read output and error files, and make sure messages are inside.
  const Result<string> output = os::read(outfile);
  ASSERT_SOME(output);
  EXPECT_EQ("hello\n", output.get());

  const Result<string> error = os::read(errorfile);
  ASSERT_SOME(error);
#ifdef __WINDOWS__
  EXPECT_EQ("goodbye \n", error.get());
#else
  EXPECT_EQ("goodbye\n", error.get());
#endif // __WINDOWS__
}


TEST_F(SubprocessTest, PipeOutputToPath)
{
  Try<string> testdir = os::mkdtemp();
  ASSERT_SOME(testdir);

  // Name the files to pipe output and error to.
  const string outfile_name = "out.txt";
  const string outfile = path::join(testdir.get(), outfile_name);

  const string errorfile_name = "error.txt";
  const string errorfile = path::join(testdir.get(), errorfile_name);

  // Pipe simple string to output file.
  run_subprocess(
      [outfile]() -> Try<Subprocess> {
        return subprocess(
            "echo hello",
            Subprocess::FD(STDIN_FILENO),
            Subprocess::PATH(outfile),
            Subprocess::FD(STDERR_FILENO));
      });

  // Pipe simple string to error file.
  run_subprocess(
      [errorfile]() -> Try<Subprocess> {
        return subprocess(
            "echo goodbye 1>&2",
            Subprocess::FD(STDIN_FILENO),
            Subprocess::FD(STDOUT_FILENO),
            Subprocess::PATH(errorfile));
      });

  // Finally, read output and error files, and make sure messages are inside.
  const Result<string> output = os::read(outfile);
  ASSERT_SOME(output);
  EXPECT_EQ("hello\n", output.get());

  const Result<string> error = os::read(errorfile);
  ASSERT_SOME(error);
#ifdef __WINDOWS__
  EXPECT_EQ("goodbye \n", error.get());
#else
  EXPECT_EQ("goodbye\n", error.get());
#endif // __WINDOWS__
}


TEST_F(SubprocessTest, EnvironmentEcho)
{
  Try<string> testdir = os::mkdtemp();
  ASSERT_SOME(testdir);

  // Name the file to pipe output to.
  const string outfile_name = "out.txt";
  const string outfile = path::join(testdir.get(), outfile_name);

  // Pipe simple string to output file.
  run_subprocess(
      [outfile]() -> Try<Subprocess> {
        const map<string, string> environment =
          {
            { "key1", "value1" },
            { "key2", "value2" }
          };

        const string shell_command =
#ifdef __WINDOWS__
          "echo %key2%";
#else
          "echo $key2";
#endif // __WINDOWS__

        return subprocess(
            shell_command,
            Subprocess::FD(STDIN_FILENO),
            Subprocess::PATH(outfile),
            Subprocess::FD(STDERR_FILENO),
            environment);
      });

  // Finally, read output file, and make sure message is inside.
  const Result<string> output = os::read(outfile);
  ASSERT_SOME(output);
  EXPECT_EQ("value2\n", output.get());
}


// NOTE: These tests can't be run on Windows because the rely on functionality
// that does not exist on Windows. For example, `os::nonblock` will not work on
// all file descriptors on Windows.
#ifndef __WINDOWS__
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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

  AWAIT_EXPECT_WEXITSTATUS_EQ(1, s.get().status());

  // SIGTERM.
  s = subprocess(SLEEP_COMMAND(60));

  ASSERT_SOME(s);

  kill(s.get().pid(), SIGTERM);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WTERMSIG_EQ(SIGTERM, s.get().status());

  // SIGKILL.
  s = subprocess(SLEEP_COMMAND(60));

  ASSERT_SOME(s);

  kill(s.get().pid(), SIGKILL);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, s.get().status());
}


TEST_F(SubprocessTest, PipeOutput)
{
  // Standard out.
  Try<Subprocess> s = subprocess(
      "echo hello",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

  // Standard error.
  s = subprocess(
      "echo hello 1>&2",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}


TEST_F(SubprocessTest, PipeInput)
{
  Try<Subprocess> s = subprocess(
      "read word ; echo $word",
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}


TEST_F(SubprocessTest, PipeRedirect)
{
  Try<Subprocess> s = subprocess(
      "echo 'hello world'",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(s);

  // Create a temporary file for splicing into.
  string path = path::join(os::getcwd(), "stdout");

  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);
  ASSERT_SOME(os::nonblock(fd.get()));

  ASSERT_SOME(s.get().out());
  ASSERT_SOME(os::nonblock(s.get().out().get()));
  AWAIT_READY(io::redirect(s.get().out().get(), fd.get()));

  // Close our copy of the fd.
  EXPECT_SOME(os::close(fd.get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PATH(out),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

  Try<string> read = os::read(out);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  // Standard error.
  s = subprocess(
      "echo hello 1>&2",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::PATH(err));

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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
      Subprocess::FD(STDERR_FILENO));

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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(outFd.get()),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(os::close(outFd.get()));
  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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
      Subprocess::FD(STDERR_FILENO));

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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}
#endif // __WINDOWS__


struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    add(&Flags::b, "b", "bool");
    add(&Flags::i, "i", "int");
    add(&Flags::s, "s", "string");
    add(&Flags::s2, "s2", "string with single quote");
    add(&Flags::s3, "s3", "string with double quote");
    add(&Flags::d, "d", "Duration");
    add(&Flags::y, "y", "Bytes");
    add(&Flags::j, "j", "JSON::Object");
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


// NOTE: These tests can't be run on Windows because the rely on functionality
// that does not exist on Windows. For example, `os::nonblock` will not work on
// all file descriptors on Windows.
#ifndef __WINDOWS__
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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PATH(out),
      Subprocess::FD(STDERR_FILENO),
      &flags);

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s.get().status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

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
  Try<flags::Warnings> load = flags2.load(None(), argc, argv);
  ASSERT_SOME(load);
  EXPECT_EQ(0u, load->warnings.size());

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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());

  // Multiple key-value pairs.
  environment.clear();
  environment["MESSAGE0"] = "hello";
  environment["MESSAGE1"] = "world";

  s = subprocess(
      "echo $MESSAGE0 $MESSAGE1",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}


TEST_F(SubprocessTest, EnvironmentWithSpaces)
{
  // Spaces in value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello world";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}


TEST_F(SubprocessTest, EnvironmentWithSpacesAndQuotes)
{
  // Spaces and quotes in value.
  map<string, string> environment;
  environment["MESSAGE"] = "\"hello world\"";

  Try<Subprocess> s = subprocess(
      "echo $MESSAGE",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
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
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
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

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s.get().status());
}
#endif // __WINDOWS__


// TODO(joerg84): Consider adding tests for setsid, working_directory,
// and supervisor childHooks.
