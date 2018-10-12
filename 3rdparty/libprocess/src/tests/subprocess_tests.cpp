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
using process::Future;
using process::MAX_REAP_INTERVAL;
using process::Subprocess;
using process::subprocess;

using std::map;
using std::shared_ptr;
using std::string;
using std::vector;


class SubprocessTest: public TemporaryDirectoryTest {};


void run_subprocess(const lambda::function<Try<Subprocess>()>& createSubprocess)
{
  Try<Subprocess> s = createSubprocess();

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  // Check process exited cleanly.
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


// Calls subprocess, pipes output to an open file descriptor (and specifically
// a file descriptor for a file, rather than a socket).
TEST_F(SubprocessTest, PipeOutputToFileDescriptor)
{
  // Create temporary files to pipe `stdin` to, and open it. We will pipe
  // output into this file.
  const string outfile_name = "out.txt";
  const string outfile = path::join(sandbox.get(), outfile_name);
  ASSERT_SOME(os::touch(outfile));

  Try<int_fd> outfile_fd = os::open(outfile, O_RDWR);
  ASSERT_SOME(outfile_fd);

  // Create temporary files to pipe `stderr` to, and open it. We will pipe
  // error into this file.
  const string errorfile_name = "error.txt";
  const string errorfile = path::join(sandbox.get(), errorfile_name);
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
  // Name the files to pipe output and error to.
  const string outfile_name = "out.txt";
  const string outfile = path::join(sandbox.get(), outfile_name);

  const string errorfile_name = "error.txt";
  const string errorfile = path::join(sandbox.get(), errorfile_name);

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
  // Name the file to pipe output to.
  const string outfile_name = "out.txt";
  const string outfile = path::join(sandbox.get(), outfile_name);

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


TEST_F(SubprocessTest, Status)
{
  // Exit 0.
  Try<Subprocess> s = subprocess("exit 0");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  // Exit 1.
  s = subprocess("exit 1");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(1, s->status());

  // NOTE: This part of the test does not run on Windows because
  // Windows does not use `SIGTERM` etc. to kill processes.
#ifndef __WINDOWS__
  // SIGTERM.
  s = subprocess(SLEEP_COMMAND(60));

  ASSERT_SOME(s);

  kill(s->pid(), SIGTERM);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WTERMSIG_EQ(SIGTERM, s->status());

  // SIGKILL.
  s = subprocess(SLEEP_COMMAND(60));

  ASSERT_SOME(s);

  kill(s->pid(), SIGKILL);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, s->status());
#endif // __WINDOWS__
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
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  // Standard error.
  s = subprocess(
      "echo hello 1>&2",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::PIPE());

  ASSERT_SOME(s);
  ASSERT_SOME(s->err());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello \r\n", io::read(s->err().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->err().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


// This test checks that we can open a subprocess, have it write a
// substantial amount of data (two memory pages) to a pipe held by the
// parent process (this test) without hanging, and then check that the
// process exits and is reaped correctly.
TEST_F(SubprocessTest, PipeLargeOutput)
{
  const string output(2 * os::pagesize(), 'c');
  const string outfile = path::join(sandbox.get(), "out.txt");
  ASSERT_SOME(os::write(outfile, output));

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "type " + outfile,
#else
      "cat " + outfile,
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());

#ifdef __WINDOWS__
  ::SetLastError(0);
#endif // __WINDOWS__

  // Read 1 more than the input size, so we can trigger the EOF error
  // on Windows.
  EXPECT_SOME_EQ(output, os::read(s->out().get(), 1 + output.size()));

#ifdef __WINDOWS__
  // NOTE: On Windows, this is the end-of-file condition when reading
  // from a pipe being written to by a child process. When it finishes
  // writing, the last read will successfully return all the data, and
  // the Windows error will be set to this.
  EXPECT_EQ(::GetLastError(), ERROR_BROKEN_PIPE);
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  // NOTE: Because we are specifically writing more data (two pages)
  // than can be held by the OS-allocated buffer, (on Windows this is
  // one page), we cannot reap the process before reading because it
  // will not exit until it has written all its data. It can only
  // successfully write all its data if we read it in the parent
  // process, otherwise the buffer fills up, and the OS makes the
  // process wait until the buffer is emptied.

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, PipeInput)
{
  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "powershell.exe",
      {"powershell.exe", "-NoProfile", "-Command", "[Console]::In.Readline()"},
#else
      "read word ; echo $word",
#endif // __WINDOWS__
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(s);
  ASSERT_SOME(s->in());
  ASSERT_SOME(os::write(s->in().get(), "hello\n"));

  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
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
  ASSERT_SOME(io::prepare_async(fd.get()));

  ASSERT_SOME(s->out());
  ASSERT_SOME(io::prepare_async(s->out().get()));
  AWAIT_READY(io::redirect(s->out().get(), fd.get()));

  // Close our copy of the fd.
  EXPECT_SOME(os::close(fd.get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  // Now make sure all the data is there!
  Try<string> read = os::read(path);
  ASSERT_SOME(read);
#ifdef __WINDOWS__
  EXPECT_EQ("'hello world'\n", read.get());
#else
  EXPECT_EQ("hello world\n", read.get());
#endif // __WINDOWS__
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
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

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
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  read = os::read(err);
  ASSERT_SOME(read);
#ifdef __WINDOWS__
  EXPECT_EQ("hello \n", read.get());
#else
  EXPECT_EQ("hello\n", read.get());
#endif // __WINDOWS__
}


TEST_F(SubprocessTest, PathInput)
{
  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "powershell.exe",
      {"powershell.exe", "-NoProfile", "-Command", "[Console]::In.Readline()"},
#else
      "read word ; echo $word",
#endif // __WINDOWS__
      Subprocess::PATH(in),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, FdOutput)
{
  string out = path::join(os::getcwd(), "stdout");
  string err = path::join(os::getcwd(), "stderr");

  // Standard out.
  Try<int_fd> outFd = os::open(
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
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  Try<string> read = os::read(out);
  ASSERT_SOME(read);
  EXPECT_EQ("hello\n", read.get());

  // Standard error.
  Try<int_fd> errFd = os::open(
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
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  read = os::read(err);
  ASSERT_SOME(read);
#ifdef __WINDOWS__
  EXPECT_EQ("hello \n", read.get());
#else
  EXPECT_EQ("hello\n", read.get());
#endif // __WINDOWS__
}


TEST_F(SubprocessTest, FdInput)
{
  string in = path::join(os::getcwd(), "stdin");

  ASSERT_SOME(os::write(in, "hello\n"));

  Try<int_fd> inFd = os::open(in, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(inFd);

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "powershell.exe",
      {"powershell.exe", "-NoProfile", "-Command", "[Console]::In.Readline()"},
#else
      "read word ; echo $word",
#endif // __WINDOWS__
      Subprocess::FD(inFd.get()),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  ASSERT_SOME(os::close(inFd.get()));

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, Default)
{
  Try<Subprocess> s = subprocess("echo hello world");

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


namespace {

struct TestFlags : public virtual flags::FlagsBase
{
  TestFlags()
  {
    add(&TestFlags::b, "b", "bool");
    add(&TestFlags::i, "i", "int");
    add(&TestFlags::s, "s", "string");
    add(&TestFlags::s2, "s2", "string with single quote");
    add(&TestFlags::s3, "s3", "string with double quote");
    add(&TestFlags::d, "d", "Duration");
    add(&TestFlags::y, "y", "Bytes");
    add(&TestFlags::j, "j", "JSON::Object");
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

} // namespace {


TEST_F(SubprocessTest, Flags)
{
  TestFlags flags;
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

#ifdef __WINDOWS__
  // The Windows version of `echo` is a built-in of the command
  // prompt, and it simply reproduces the entire command line string.
  // However, the flags class (and thus this test) is expecting the
  // semantics of a native binary interpreting the command line
  // arguments via the Windows API `CommandLineToArgv`. When a regular
  // Windows application (in contrast to `echo`) gets command line
  // arguments, the text is processed automatically by
  // `CommandLineToArgv`, which converts the command line string into
  // an array. For example, this is the output of `echo`:
  //
  //    > cmd.exe /c echo "--s3=\"geek\""
  //    "--s3=\"geek\""
  //
  // With `test-echo.exe`, a small native binary that just prints its
  // arguments, the output is:
  //
  //     > test-echo.exe "--s3=\"geek\""
  //     --s3="geek"
  //
  // This is the behavior expected by the test as the POSIX version of
  // `echo` is a native binary.
  string test_echo_path = path::join(BUILD_DIR, "test-echo.exe");
#endif

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      test_echo_path,
      {test_echo_path},
#else
      "/bin/echo",
      vector<string>(1, "echo"),
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PATH(out),
      Subprocess::FD(STDERR_FILENO),
      &flags);

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

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

  TestFlags flags2;
  Try<flags::Warnings> load = flags2.load(None(), argc, argv);
  ASSERT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

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
#ifdef __WINDOWS__
      "echo %MESSAGE%",
#else
      "echo $MESSAGE",
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());

  // Multiple key-value pairs.
  environment.clear();
  environment["MESSAGE0"] = "hello";
  environment["MESSAGE1"] = "world";

  s = subprocess(
#ifdef __WINDOWS__
      "echo %MESSAGE0% %MESSAGE1%",
#else
      "echo $MESSAGE0 $MESSAGE1",
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello world\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello world\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, EnvironmentWithSpaces)
{
  // Spaces in value.
  map<string, string> environment;
  environment["MESSAGE"] = "hello world";

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "echo %MESSAGE%",
#else
      "echo $MESSAGE",
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("hello world\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("hello world\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, EnvironmentWithSpacesAndQuotes)
{
  // Spaces and quotes in value.
  map<string, string> environment;
  environment["MESSAGE"] = "\"hello world\"";

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "echo %MESSAGE%",
#else
      "echo $MESSAGE",
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("\"hello world\"\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("\"hello world\"\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


TEST_F(SubprocessTest, EnvironmentOverride)
{
  // Ensure we override an existing environment variable.
  os::setenv("MESSAGE1", "hello");
  os::setenv("MESSAGE2", "world");

  map<string, string> environment;
  environment["MESSAGE2"] = "goodbye";

  Try<Subprocess> s = subprocess(
#ifdef __WINDOWS__
      "echo %MESSAGE1% %MESSAGE2%",
#else
      "echo $MESSAGE1 $MESSAGE2",
#endif // __WINDOWS__
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      environment);

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());
  // NOTE: Windows will emit `%VAR%` if the environment variable `VAR`
  // was not defined, unlike POSIX which will emit nothing.
#ifdef __WINDOWS__
  AWAIT_EXPECT_EQ("%MESSAGE1% goodbye\r\n", io::read(s->out().get()));
#else
  AWAIT_EXPECT_EQ("goodbye\n", io::read(s->out().get()));
#endif // __WINDOWS__

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


#ifdef __linux__
// This test verifies:
//   1. The subprocess will have the stdio file descriptors.
//   2. The whitelisted file descriptors will be successfully
//      inherited by the subprocess.
//   3. The non-whitelisted file descriptors will be not be
//      inherited by the subprocess.
TEST_F(SubprocessTest, WhiteListFds)
{
  Try<int_fd> fd1 = os::open(
      path::join(os::getcwd(), id::UUID::random().toString()),
      O_CREAT | O_EXCL | O_RDONLY | O_CLOEXEC);

  Try<int_fd> fd2 = os::open(
      path::join(os::getcwd(), id::UUID::random().toString()),
      O_CREAT | O_EXCL | O_RDONLY);

  ASSERT_SOME(fd1);
  ASSERT_SOME(fd2);

  Try<Subprocess> s = subprocess(
      "ls /dev/fd",
      Subprocess::FD(STDIN_FILENO),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      {},
      {},
      {fd1.get()});

  ASSERT_SOME(s);
  ASSERT_SOME(s->out());

  Future<string> output = io::read(s->out().get());
  AWAIT_READY(output);

  hashset<int_fd> fds;

  vector<string> tokens = strings::tokenize(output.get(), "\n");
  foreach (const string& fdString, tokens) {
    Try<int_fd> fd = numify<int_fd>(fdString);
    ASSERT_SOME(fd);

    fds.insert(fd.get());
  }

  // The subprocess should always have the stdio file descriptors.
  EXPECT_TRUE(fds.contains(STDIN_FILENO));
  EXPECT_TRUE(fds.contains(STDOUT_FILENO));
  EXPECT_TRUE(fds.contains(STDERR_FILENO));

  // `fd1` should be inherited by the subprocess since it is whitelisted even
  // it has `O_CLOEXEC` set initially.
  EXPECT_TRUE(fds.contains(fd1.get()));

  // `fd2` should not be inherited by the subprocess since it is not whitelisted
  // even it has no `O_CLOEXEC` set initially.
  EXPECT_FALSE(fds.contains(fd2.get()));

  ASSERT_SOME(os::close(fd1.get()));
  ASSERT_SOME(os::close(fd2.get()));

  // Advance time until the internal reaper reaps the subprocess.
  Clock::pause();
  while (s->status().isPending()) {
    Clock::advance(MAX_REAP_INTERVAL());
    Clock::settle();
  }
  Clock::resume();

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}
#endif // __linux__


// TODO(joerg84): Consider adding tests for setsid, working_directory,
// and supervisor childHooks.
