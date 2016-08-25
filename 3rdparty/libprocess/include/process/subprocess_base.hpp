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

#ifndef __PROCESS_SUBPROCESS_BASE_HPP__
#define __PROCESS_SUBPROCESS_BASE_HPP__

#include <sys/types.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/os/shell.hpp>


namespace process {

// Flag describing whether a new process should generate a new sid.
enum Setsid
{
  SETSID,
  NO_SETSID,
};

// Flag describing whether a new process should be monitored by a seperate
// watch process and be killed in case the parent process dies.
enum Watchdog {
  MONITOR,
  NO_MONITOR,
};

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
   *   1. PIPE: Redirect to a pipe.  The pipe will be created when
   *      launching a subprocess and the user can read/write the
   *      parent side of the pipe using `Subprocess::in/out/err`.
   *   2. PATH: Redirect to a file.  For stdout/stderr, the file will
   *      be created if it does not exist.  If the file exists, it
   *      will be appended.
   *   3. FD: Redirect to an open file descriptor.
   */
  class IO
  {
  public:
    /**
     * For input file descriptors a child reads from the `read` file
     * descriptor and a parent may write to the `write` file
     * descriptor if one is present.
     *
     * NOTE: We initialize `read` to -1 so that we do not close an
     * arbitrary file descriptor,in case we encounter an error
     * while starting a subprocess (closing -1 is always a no-op).
     */
    struct InputFileDescriptors
    {
#ifndef __WINDOWS__
      int read = -1;
      Option<int> write = None();
#else
      HANDLE read = INVALID_HANDLE_VALUE;
      Option<HANDLE> write = None();
#endif // __WINDOWS__
    };

    /**
     * For output file descriptors a child write to the `write` file
     * descriptor and a parent may read from the `read` file
     * descriptor if one is present.
     *
     * NOTE: We initialize `write` to -1 so that we do not close an
     * arbitrary file descriptor,in case we encounter an error
     * while starting a subprocess (closing -1 is always a no-op).
     */
    struct OutputFileDescriptors
    {
#ifndef __WINDOWS__
      Option<int> read = None();
      int write = -1;
#else
      Option<HANDLE> read = None();
      HANDLE write = INVALID_HANDLE_VALUE;
#endif // __WINDOWS__
    };

    /**
     * Describes the lifecycle of a file descriptor passed into a subprocess
     * via the `Subprocess::FD` helper.
     */
    enum FDType {
      /**
       * The file descriptor is duplicated before being passed to the
       * subprocess.  The original file descriptor remains open.
       */
      DUPLICATED,

      /**
       * The file descriptor is not duplicated before being passed to the
       * subprocess.  Upon spawning the subprocess, the original file
       * descriptor is closed in the parent and remains open in the child.
       */
      OWNED
    };


  private:
    friend class Subprocess;

    friend Try<Subprocess> subprocess(
        const std::string& path,
        std::vector<std::string> argv,
        const Subprocess::IO& in,
        const Subprocess::IO& out,
        const Subprocess::IO& err,
        const Setsid set_sid,
        const flags::FlagsBase* flags,
        const Option<std::map<std::string, std::string>>& environment,
        const Option<lambda::function<
            pid_t(const lambda::function<int()>&)>>& clone,
        const std::vector<Subprocess::Hook>& parent_hooks,
        const Option<std::string>& working_directory,
        const Watchdog watchdog);

    IO(const lambda::function<Try<InputFileDescriptors>()>& _input,
       const lambda::function<Try<OutputFileDescriptors>()>& _output)
      : input(_input),
        output(_output) {}

    /**
     * Prepares a set of file descriptors for stdin of a subprocess.
     */
    lambda::function<Try<InputFileDescriptors>()> input;

    /**
     * Prepares a set of file descriptors for stdout/stderr of a subprocess.
     */
    lambda::function<Try<OutputFileDescriptors>()> output;
  };

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

  // Some syntactic sugar to create an IO::PIPE redirector.
  static IO PIPE();
  static IO PATH(const std::string& path);
  static IO FD(int fd, IO::FDType type = IO::DUPLICATED);

  /**
   * @return The operating system PID for this subprocess.
   */
  pid_t pid() const { return data->pid; }

  /**
   * @return File descriptor representing the parent side (i.e.,
   *     write side) of this subprocess' stdin pipe or None if no pipe
   *     was requested.
   */
#ifdef __WINDOWS__
  Option<HANDLE> in() const
#else
  Option<int> in() const
#endif // __WINDOWS__
  {
    return data->in;
  }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stdout pipe or None if no pipe was
   *     requested.
   */
#ifdef __WINDOWS__
  Option<HANDLE> out() const
#else
  Option<int> out() const
#endif // __WINDOWS__
  {
    return data->out;
  }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stderr pipe or None if no pipe was
   *     requested.
   */
#ifdef __WINDOWS__
  Option<HANDLE> err() const
#else
  Option<int> err() const
#endif // __WINDOWS__
  {
    return data->err;
  }

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
      const Setsid setsid,
      const flags::FlagsBase* flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<lambda::function<
          pid_t(const lambda::function<int()>&)>>& clone,
      const std::vector<Subprocess::Hook>& parent_hooks,
      const Option<std::string>& working_directory,
      const Watchdog watchdog);

  struct Data
  {
    ~Data()
    {
      if (in.isSome()) { os::close(in.get()); }
      if (out.isSome()) { os::close(out.get()); }
      if (err.isSome()) { os::close(err.get()); }

#ifdef __WINDOWS__
      os::close(processInformation.hProcess);
      os::close(processInformation.hThread);
#endif // __WINDOWS__
    }

    pid_t pid;

#ifdef __WINDOWS__
    PROCESS_INFORMATION processInformation;
#endif // __WINDOWS__

    // The parent side of the pipe for stdin/stdout/stderr. If the
    // IO mode is not a pipe, `None` will be stored.
    // NOTE: stdin, stdout, stderr are macros on some systems, hence
    // these names instead.
#ifdef __WINDOWS__
    Option<HANDLE> in;
    Option<HANDLE> out;
    Option<HANDLE> err;
#else
    Option<int> in;
    Option<int> out;
    Option<int> err;
#endif // __WINDOWS__

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
 * @param path Relative or absolute path in the filesytem to the
 *     executable.
 * @param argv Argument vector to pass to exec.
 * @param in Redirection specification for stdin.
 * @param out Redirection specification for stdout.
 * @param err Redirection specification for stderr.
 * @param set_sid Indicator whether the process should be placed in
 *     a new session after the 'parent_hooks' have been executed.
 * @param flags Flags to be stringified and appended to 'argv'.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @param working_directory Directory in which the process should
 *     chdir before exec after the 'parent_hooks' have been executed.
 * @param watchdog Indicator whether the new process should be monitored
 *     and killed if the parent process terminates.
 * @return The subprocess or an error if one occurred.
 */
// TODO(jmlvanre): Consider removing default argument for
// `parent_hooks` to force the caller to think about setting them.
Try<Subprocess> subprocess(
    const std::string& path,
    std::vector<std::string> argv,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Setsid set_sid = NO_SETSID,
    const flags::FlagsBase* flags = nullptr,
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::Hook>& parent_hooks =
      Subprocess::Hook::None(),
    const Option<std::string>& working_directory = None(),
    const Watchdog watchdog = NO_MONITOR);


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
 * @param set_sid Indicator whether the process should be placed in
 *     a new session after the 'parent_hooks' have been executed.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @param working_directory Directory in which the process should
 *     chdir before exec after the 'parent_hooks' have been executed.
 * @param watchdog Indicator whether the new process should be monitored
 *     and killed if the parent process terminates.
 * @return The subprocess or an error if one occurred.
 */
// TODO(jmlvanre): Consider removing default argument for
// `parent_hooks` to force the caller to think about setting them.
inline Try<Subprocess> subprocess(
    const std::string& command,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Setsid set_sid = NO_SETSID,
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::Hook>& parent_hooks =
      Subprocess::Hook::None(),
    const Option<std::string>& working_directory = None(),
    const Watchdog watchdog = NO_MONITOR)
{
  std::vector<std::string> argv = {os::Shell::arg0, os::Shell::arg1, command};

  return subprocess(
      os::Shell::name,
      argv,
      in,
      out,
      err,
      set_sid,
      nullptr,
      environment,
      clone,
      parent_hooks,
      working_directory,
      watchdog);
}

} // namespace process {

#endif // __PROCESS_SUBPROCESS_BASE_HPP__
