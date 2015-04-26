#ifndef __PROCESS_SUBPROCESS_HPP__
#define __PROCESS_SUBPROCESS_HPP__

#include <unistd.h>

#include <sys/types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

namespace process {

// Represents a fork() exec()ed subprocess. Access is provided to
// the input / output of the process, as well as the exit status.
// The input / output file descriptors are only closed after both:
//   1. The subprocess has terminated, and
//   2. There are no longer any references to the associated
//      Subprocess object.
class Subprocess
{
public:
  // Describes how the I/O is redirected for stdin/stdout/stderr.
  // One of the following three modes are supported:
  //   1. PIPE: Redirect to a pipe. The pipe will be created
  //      automatically and the user can read/write the parent side of
  //      the pipe from in()/out()/err().
  //   2. PATH: Redirect to a file. The file will be created if it
  //      does not exist. If the file exists, it will be appended.
  //   3. FD: Redirect to an open file descriptor.
  class IO
  {
  public:
    bool isPipe() const { return mode == PIPE; }
    bool isPath() const { return mode == PATH; }
    bool isFd() const { return mode == FD; }

  private:
    friend class Subprocess;

    friend Try<Subprocess> subprocess(
        const std::string& path,
        std::vector<std::string> argv,
        const Subprocess::IO& in,
        const Subprocess::IO& out,
        const Subprocess::IO& err,
        const Option<flags::FlagsBase>& flags,
        const Option<std::map<std::string, std::string>>& environment,
        const Option<lambda::function<int()>>& setup,
        const Option<lambda::function<
            pid_t(const lambda::function<int()>&)>>& clone);

    enum Mode
    {
      PIPE, // Redirect I/O to a pipe.
      PATH, // Redirect I/O to a file.
      FD,   // Redirect I/O to an open file descriptor.
    };

    IO(Mode _mode, const Option<int>& _fd, const Option<std::string>& _path)
      : mode(_mode), fd(_fd), path(_path) {}

    Mode mode;
    Option<int> fd;
    Option<std::string> path;
  };

  // Syntactic sugar to create IO descriptors.
  static IO PIPE()
  {
    return IO(IO::PIPE, None(), None());
  }

  static IO PATH(const std::string& path)
  {
    return IO(IO::PATH, None(), path);
  }

  static IO FD(int fd)
  {
    return IO(IO::FD, fd, None());
  }

  // Returns the pid for the subprocess.
  pid_t pid() const { return data->pid; }

  // The parent side of the pipe for stdin/stdout/stderr.
  Option<int> in()  const { return data->in;  }
  Option<int> out() const { return data->out; }
  Option<int> err() const { return data->err; }

  // Returns a future from process::reap of this subprocess.
  // Discarding this future has no effect on the subprocess.
  Future<Option<int>> status() const { return data->status; }

private:
  friend Try<Subprocess> subprocess(
      const std::string& path,
      std::vector<std::string> argv,
      const Subprocess::IO& in,
      const Subprocess::IO& out,
      const Subprocess::IO& err,
      const Option<flags::FlagsBase>& flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<lambda::function<int()>>& setup,
      const Option<lambda::function<
          pid_t(const lambda::function<int()>&)>>& clone);

  struct Data
  {
    ~Data()
    {
      if (in.isSome()) { os::close(in.get()); }
      if (out.isSome()) { os::close(out.get()); }
      if (err.isSome()) { os::close(err.get()); }
    }

    pid_t pid;

    // The parent side of the pipe for stdin/stdout/stderr. If the
    // mode is not PIPE, None will be stored.
    // NOTE: stdin, stdout, stderr are macros on some systems, hence
    // these names instead.
    Option<int> in;
    Option<int> out;
    Option<int> err;

    Future<Option<int>> status;
  };

  Subprocess() : data(new Data()) {}

  std::shared_ptr<Data> data;
};


// The Environment is combined with the OS environment and overrides
// where necessary.
// The setup function is run after forking but before executing the
// command. If the return value of that setup function is non-zero,
// then that is what the subprocess status will be;
// status = setup && command.
// NOTE: Take extra care about the design of the setup function as it
// must not contain any async unsafe code.
// TODO(dhamon): Add an option to not combine the two environments.
Try<Subprocess> subprocess(
    const std::string& path,
    std::vector<std::string> argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const Option<flags::FlagsBase>& flags = None(),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None());


inline Try<Subprocess> subprocess(
    const std::string& path,
    std::vector<std::string> argv,
    const Option<flags::FlagsBase>& flags = None(),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None())
{
  return subprocess(
      path,
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      flags,
      environment,
      setup,
      clone);
}


// Overloads for launching a shell command. Currently, we do not
// support flags for shell command variants due to the complexity
// involved in escaping quotes in flags.
inline Try<Subprocess> subprocess(
    const std::string& command,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None())
{
  std::vector<std::string> argv(3);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = command;

  return subprocess(
      "/bin/sh",
      argv,
      in,
      out,
      err,
      None(),
      environment,
      setup,
      clone);
}


inline Try<Subprocess> subprocess(
    const std::string& command,
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None())
{
  return subprocess(
      command,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      environment,
      setup,
      clone);
}

} // namespace process {

#endif // __PROCESS_SUBPROCESS_HPP__
