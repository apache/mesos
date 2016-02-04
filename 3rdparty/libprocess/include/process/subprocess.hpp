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

#ifndef __PROCESS_SUBPROCESS_HPP__
#define __PROCESS_SUBPROCESS_HPP__

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <sys/types.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace process {

/**
 * Represents a fork() exec()ed subprocess. Access is provided to the
 * input / output of the process, as well as the exit status. The
 * input / output file descriptors are only closed after:
 *   1. The subprocess has terminated.
 *   2. There are no longer any references to the associated
 *      Subprocess object.
 */
class Subprocess
{
public:
  // Forward declarations.
  struct Hook;

  /**
   * Describes how the I/O is redirected for stdin/stdout/stderr.
   * One of the following three modes are supported:
   *   1. PIPE: Redirect to a pipe. The pipe will be created
   *      automatically and the user can read/write the parent side of
   *      the pipe from in()/out()/err().
   *   2. PATH: Redirect to a file. The file will be created if it
   *      does not exist. If the file exists, it will be appended.
   *   3. FD: Redirect to an open file descriptor.
   */
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
            pid_t(const lambda::function<int()>&)>>& clone,
        const std::vector<Subprocess::Hook>& parent_hooks);

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

  /**
   * Provides some syntactic sugar to create an IO::PIPE redirector.
   *
   * @return An IO::PIPE redirector.
   */
  static IO PIPE()
  {
    return IO(IO::PIPE, None(), None());
  }

  /**
   * Provides some syntactic sugar to create an IO::PATH redirector.
   *
   * @return An IO::PATH redirector.
   */
  static IO PATH(const std::string& path)
  {
    return IO(IO::PATH, None(), path);
  }

  /**
   * Provides some syntactic sugar to create an IO::FD redirector.
   *
   * @return An IO::FD redirector.
   */
  static IO FD(int fd)
  {
    return IO(IO::FD, fd, None());
  }

  /**
   * A hook can be passed to a `subprocess` call. It provides a way to
   * inject dynamic implementation behavior between the clone and exec
   * calls in the implementation of `subprocess`.
   */
  struct Hook
  {
    /**
     * Returns an empty list of hooks.
     */
    static std::vector<Hook> None() { return std::vector<Hook>(); }

    Hook(const lambda::function<Try<Nothing>(pid_t)>& _parent_callback);

    /**
     * The callback that must be sepcified for execution after the
     * child has been cloned, but before it start executing the new
     * process. This provides access to the child pid after its
     * initialization to add tracking or modify execution state of
     * the child before it executes the new process.
     */
    const lambda::function<Try<Nothing>(pid_t)> parent_callback;

    friend class Subprocess;
  };

  /**
   * @return The operating system PID for this subprocess.
   */
  pid_t pid() const { return data->pid; }

  /**
   * @return File descriptor representing the parent side (i.e.,
   *     write side) of this subprocess' stdin pipe or None if no pipe
   *     was requested.
   */
  Option<int> in()  const { return data->in;  }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stdout pipe or None if no pipe was
   *     requested.
   */
  Option<int> out() const { return data->out; }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stderr pipe or None if no pipe was
   *     requested.
   */
  Option<int> err() const { return data->err; }

  /**
   * Exit status of this subprocess captured as a Future (completed
   * when the subprocess exits).
   *
   * The exit status is propagated from an underlying call to
   * 'waitpid' and can be used with macros defined in wait.h, i.e.,
   * 'WIFEXITED(status)'.
   *
   * NOTE: Discarding this future has no effect on the subprocess!
   *
   * @return Future from doing a process::reap of this subprocess.
   */
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
          pid_t(const lambda::function<int()>&)>>& clone,
      const std::vector<Subprocess::Hook>& parent_hooks);

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


/**
 * Forks a subprocess and execs the specified 'path' with the
 * specified 'argv', redirecting stdin, stdout, and stderr as
 * specified by 'in', 'out', and 'err' respectively.
 *
 * If 'setup' is not None, runs the specified function after forking
 * but before exec'ing. If the return value of 'setup' is non-zero
 * then that gets returned in 'status()' and we will not exec.
 *
 * @param path Relative or absolute path in the filesytem to the
 *     executable.
 * @param argv Argument vector to pass to exec.
 * @param in Redirection specification for stdin.
 * @param out Redirection specification for stdout.
 * @param err Redirection specification for stderr.
 * @param flags Flags to be stringified and appended to 'argv'.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param setup Function to be invoked after forking but before
 *     exec'ing. NOTE: Take extra care not to invoke any
 *     async unsafe code in the body of this function.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @return The subprocess or an error if one occured.
 */
// TODO(jmlvanre): Consider removing default argument for
// `parent_hooks` to force the caller to think about setting them.
Try<Subprocess> subprocess(
    const std::string& path,
    std::vector<std::string> argv,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Option<flags::FlagsBase>& flags = None(),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::Hook>& parent_hooks =
      Subprocess::Hook::None());


/**
 * Overload of 'subprocess' for launching a shell command, i.e., 'sh
 * -c command'.
 *
 * Currently, we do not support flags for shell command variants due
 * to the complexity involved in escaping quotes in flags.
 *
 * @param command Shell command to execute.
 * @param in Redirection specification for stdin.
 * @param out Redirection specification for stdout.
 * @param err Redirection specification for stderr.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param setup Function to be invoked after forking but before
 *     exec'ing. NOTE: Take extra care not to invoke any
 *     async unsafe code in the body of this function.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @return The subprocess or an error if one occured.
 */
// TODO(jmlvanre): Consider removing default argument for
// `parent_hooks` to force the caller to think about setting them.
inline Try<Subprocess> subprocess(
    const std::string& command,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::Hook>& parent_hooks =
      Subprocess::Hook::None())
{
  std::vector<std::string> argv = {"sh", "-c", command};

  return subprocess(
      "sh",
      argv,
      in,
      out,
      err,
      None(),
      environment,
      setup,
      clone,
      parent_hooks);
}

} // namespace process {

#endif // __PROCESS_SUBPROCESS_HPP__
