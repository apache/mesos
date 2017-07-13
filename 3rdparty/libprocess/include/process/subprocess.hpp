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

#ifndef __PROCESS_SUBPROCESS_HPP__
#define __PROCESS_SUBPROCESS_HPP__

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/os/shell.hpp>
#include <stout/os/int_fd.hpp>


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
  struct ParentHook;
  class ChildHook;

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
     * arbitrary file descriptor, in case we encounter an error
     * while starting a subprocess (closing -1 is always a no-op).
     */
    struct InputFileDescriptors
    {
      int_fd read = -1;
      Option<int_fd> write = None();
    };

    /**
     * For output file descriptors a child writes to the `write` file
     * descriptor and a parent may read from the `read` file
     * descriptor if one is present.
     *
     * NOTE: We initialize `write` to -1 so that we do not close an
     * arbitrary file descriptor, in case we encounter an error
     * while starting a subprocess (closing -1 is always a no-op).
     */
    struct OutputFileDescriptors
    {
      Option<int_fd> read = None();
      int_fd write = -1;
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
        const flags::FlagsBase* flags,
        const Option<std::map<std::string, std::string>>& environment,
        const Option<lambda::function<
            pid_t(const lambda::function<int()>&)>>& clone,
        const std::vector<Subprocess::ParentHook>& parent_hooks,
        const std::vector<Subprocess::ChildHook>& child_hooks);

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
   * calls in the parent process.
   */
  struct ParentHook
  {
    ParentHook(const lambda::function<Try<Nothing>(pid_t)>& _parent_setup);

    /**
     * The callback that must be specified for execution after the
     * child has been cloned, but before it starts executing the new
     * process. This provides access to the child pid after its
     * initialization to add tracking or modify execution state of
     * the child before it executes the new process.
     */
    const lambda::function<Try<Nothing>(pid_t)> parent_setup;

    friend class Subprocess;

#ifdef __WINDOWS__
    /**
     * A Windows Job Object is used to manage groups of processes, which
     * we use due to the lack of a process hierarchy (in a UNIX sense)
     * on Windows.
     *
     * This hook places the subprocess into a Job Object, which will allow
     * us to kill the subprocess and all of its children together.
     */
    static ParentHook CREATE_JOB();
#endif // __WINDOWS__
  };

  /**
   * A `ChildHook` can be passed to a `subprocess` call. It provides a way to
   * inject predefined behavior between the clone and exec calls in the
   * child process.
   * As such `ChildHooks` have to fulfill certain criteria (especially
   * being async safe) the class does not offer a public constructor.
   * Instead instances can be created via factory methods.
   * NOTE: Returning an error from a childHook causes the child process to
   * abort.
   */
  class ChildHook
  {
  public:
    /**
     * `ChildHook` for changing the working directory.
     */
    static ChildHook CHDIR(const std::string& working_directory);

    /**
     * `ChildHook` for generating a new session id.
     */
    static ChildHook SETSID();

#ifndef __WINDOWS__
    /**
     * `ChildHook` for duplicating a file descriptor.
     */
    static ChildHook DUP2(int oldFd, int newFd);

    /**
     * `ChildHook` to unset CLOEXEC on a file descriptor. This is
     * useful to explicitly pass an FD to a subprocess.
     */
    static ChildHook UNSET_CLOEXEC(int fd);
#endif // __WINDOWS__

    /**
     * `ChildHook` for starting a Supervisor process monitoring
     *  and killing the child process if the parent process terminates.
     *
     * NOTE: The supervisor process sets the process group id in order for it
     * and its child processes to be killed together. We should not (re)set the
     * sid after this.
     */
    static ChildHook SUPERVISOR();

    Try<Nothing> operator()() const { return child_setup(); }

  private:
    ChildHook(const lambda::function<Try<Nothing>()>& _child_setup);

    const lambda::function<Try<Nothing>()> child_setup;
  };

  // Some syntactic sugar to create an IO::PIPE redirector.
  static IO PIPE();
  static IO PATH(const std::string& path);
  static IO FD(int_fd fd, IO::FDType type = IO::DUPLICATED);

  /**
   * @return The operating system PID for this subprocess.
   */
  pid_t pid() const { return data->pid; }

  /**
   * @return File descriptor representing the parent side (i.e.,
   *     write side) of this subprocess' stdin pipe or None if no pipe
   *     was requested.
   */
  Option<int_fd> in() const
  {
    return data->in;
  }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stdout pipe or None if no pipe was
   *     requested.
   */
  Option<int_fd> out() const
  {
    return data->out;
  }

  /**
   * @return File descriptor representing the parent side (i.e., write
   *     side) of this subprocess' stderr pipe or None if no pipe was
   *     requested.
   */
  Option<int_fd> err() const
  {
    return data->err;
  }

  /**
   * Exit status of this subprocess captured as a Future (completed
   * when the subprocess exits).
   *
   * On Posix, the exit status is propagated from an underlying call
   * to `waitpid` and can be used with macros defined in wait.h, i.e.,
   * `WIFEXITED(status)`.
   *
   * On Windows, the exit status contains the exit code from an
   * underlying call to `GetExitCodeProcess()`.
   *
   * TODO(alexr): Ensure the code working with `status` is portable by
   * either making `WIFEXITED` family macros no-op on Windows or
   * converting `status` to a tuple <termination status, exit code>,
   * see MESOS-7242.
   *
   * NOTE: Discarding this future has no effect on the subprocess!
   *
   * @return Future from doing a `process::reap()` of this subprocess.
   *     Note that `process::reap()` never fails or discards this future.
   */
  Future<Option<int>> status() const { return data->status; }

private:
  friend Try<Subprocess> subprocess(
      const std::string& path,
      std::vector<std::string> argv,
      const Subprocess::IO& in,
      const Subprocess::IO& out,
      const Subprocess::IO& err,
      const flags::FlagsBase* flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<lambda::function<
          pid_t(const lambda::function<int()>&)>>& clone,
      const std::vector<Subprocess::ParentHook>& parent_hooks,
      const std::vector<Subprocess::ChildHook>& child_hooks);

  struct Data
  {
    ~Data()
    {
      if (in.isSome()) { os::close(in.get()); }
      if (out.isSome()) { os::close(out.get()); }
      if (err.isSome()) { os::close(err.get()); }
    }

    pid_t pid;

#ifdef __WINDOWS__
    Option<::internal::windows::ProcessData> process_data;
#endif // __WINDOWS__

    // The parent side of the pipe for stdin/stdout/stderr. If the
    // IO mode is not a pipe, `None` will be stored.
    // NOTE: stdin, stdout, stderr are macros on some systems, hence
    // these names instead.
    Option<int_fd> in;
    Option<int_fd> out;
    Option<int_fd> err;

    Future<Option<int>> status;
  };

  Subprocess() : data(new Data()) {}

  std::shared_ptr<Data> data;
};

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;

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
 * @param flags Flags to be stringified and appended to 'argv'.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @param child_hooks Hooks that will be executed in the child
 *     before the child execs but after parent_hooks have executed.
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
    const flags::FlagsBase* flags = nullptr,
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::ParentHook>& parent_hooks = {},
    const std::vector<Subprocess::ChildHook>& child_hooks = {});


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
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @param parent_hooks Hooks that will be executed in the parent
 *     before the child execs.
 * @param child_hooks Hooks that will be executed in the child
 *     before the child execs but after parent_hooks have executed.
 * @return The subprocess or an error if one occurred.
 */
// TODO(jmlvanre): Consider removing default argument for
// `parent_hooks` to force the caller to think about setting them.
inline Try<Subprocess> subprocess(
    const std::string& command,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None(),
    const std::vector<Subprocess::ParentHook>& parent_hooks = {},
    const std::vector<Subprocess::ChildHook>& child_hooks = {})
{
  std::vector<std::string> argv = {os::Shell::arg0, os::Shell::arg1, command};

  return subprocess(
      os::Shell::name,
      argv,
      in,
      out,
      err,
      nullptr,
      environment,
      clone,
      parent_hooks,
      child_hooks);
}

} // namespace process {

#endif // __PROCESS_SUBPROCESS_HPP__
