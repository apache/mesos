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
#include <stdio.h>
#include <stdlib.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__
#include <sys/types.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif // __linux__
#include <sys/types.h>

#include <initializer_list>
#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/chdir.hpp>
#include <stout/os/close.hpp>
#include <stout/os/dup.hpp>
#include <stout/os/exec.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/signals.hpp>

#ifdef __WINDOWS__
#include "windows/subprocess.hpp"
#else
#include "posix/subprocess.hpp"
#endif // __WINDOWS__

using std::map;
using std::string;
using std::vector;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;


Subprocess::ParentHook::ParentHook(
    const lambda::function<Try<Nothing>(pid_t)>& _parent_setup)
  : parent_setup(_parent_setup) {}


Subprocess::ChildHook::ChildHook(
    const lambda::function<Try<Nothing>()>& _child_setup)
  : child_setup(_child_setup) {}


Subprocess::ChildHook Subprocess::ChildHook::CHDIR(
    const std::string& working_directory)
{
  return Subprocess::ChildHook([working_directory]() -> Try<Nothing> {
    const Try<Nothing> result = os::chdir(working_directory);
    if (result.isError()) {
      return Error(result.error());
    }

    return Nothing();
  });
}


Subprocess::ChildHook Subprocess::ChildHook::SETSID()
{
  return Subprocess::ChildHook([]() -> Try<Nothing> {
    // TODO(josephw): By default, child processes on Windows do not
    // terminate when the parent terminates. We need to implement
    // `JobObject` support to change this default.
#ifndef __WINDOWS__
    // Put child into its own process session to prevent the parent
    // suicide on child process SIGKILL/SIGTERM.
    if (::setsid() == -1) {
      return Error("Could not setsid");
    }
#endif // __WINDOWS__

    return Nothing();
  });
}


#ifndef __WINDOWS__
Subprocess::ChildHook Subprocess::ChildHook::DUP2(int oldFd, int newFd)
{
  return Subprocess::ChildHook([oldFd, newFd]() -> Try<Nothing> {
    return os::dup2(oldFd, newFd);
  });
}
#endif // __WINDOWS__


#ifdef __linux__
static void signalHandler(int signal)
{
  // Send SIGKILL to every process in the process group of the
  // calling process.
  kill(0, SIGKILL);
  _exit(EXIT_FAILURE);
}
#endif // __linux__


Subprocess::ChildHook Subprocess::ChildHook::SUPERVISOR()
{
  return Subprocess::ChildHook([]() -> Try<Nothing> {
#ifdef __linux__
    // Send SIGTERM to the current process if the parent exits.
    // NOTE:: This function should always succeed because we are passing
    // in a valid signal.
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Put the current process into a separate process group so that
    // we can kill it and all its children easily.
    if (setpgid(0, 0) != 0) {
      return Error("Could not start supervisor process.");
    }

    // Install a SIGTERM handler which will kill the current process
    // group. Since we already setup the death signal above, the
    // signal handler will be triggered when the parent exits.
    if (os::signals::install(SIGTERM, &signalHandler) != 0) {
      return Error("Could not start supervisor process.");
    }

    pid_t pid = fork();
    if (pid == -1) {
      return Error("Could not start supervisor process.");
    } else if (pid == 0) {
      // Child. This is the process that is going to exec the
      // process if zero is returned.

      // We setup death signal for the process as well in case
      // someone, though unlikely, accidentally kill the parent of
      // this process (the bookkeeping process).
      prctl(PR_SET_PDEATHSIG, SIGKILL);

      // NOTE: We don't need to clear the signal handler explicitly
      // because the subsequent 'exec' will clear them.
      return Nothing();
    } else {
      // Parent. This is the bookkeeping process which will wait for
      // the child process to finish.

      // Close the files to prevent interference on the communication
      // between the parent and the child process.
      ::close(STDIN_FILENO);
      ::close(STDOUT_FILENO);
      ::close(STDERR_FILENO);

      // Block until the child process finishes.
      int status = 0;
      while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
          _exit(EXIT_FAILURE);
        }
      }

      // Forward the exit status if the child process exits normally.
      if (WIFEXITED(status)) {
        _exit(WEXITSTATUS(status));
      }

      _exit(EXIT_FAILURE);
    }
#endif // __linux__
    return Nothing();
  });
}


Subprocess::IO Subprocess::FD(int_fd fd, IO::FDType type)
{
  return Subprocess::IO(
      [fd, type]() -> Try<InputFileDescriptors> {
        int_fd prepared_fd = -1;
        switch (type) {
          case IO::DUPLICATED: {
            Try<int_fd> dup = os::dup(fd);
            if (dup.isError()) {
              return Error(dup.error());
            }

            prepared_fd = dup.get();
            break;
          }
          case IO::OWNED: {
            prepared_fd = fd;
            break;
          }

            // NOTE: By not setting a default we leverage the compiler
            // errors when the enumeration is augmented to find all
            // the cases we need to provide. Same for below.
        }

        InputFileDescriptors fds;
        fds.read = prepared_fd;
        return fds;
      },
      [fd, type]() -> Try<OutputFileDescriptors> {
        int_fd prepared_fd = -1;
        switch (type) {
          case IO::DUPLICATED: {
            Try<int_fd> dup = os::dup(fd);
            if (dup.isError()) {
              return Error(dup.error());
            }

            prepared_fd = dup.get();
            break;
          }
          case IO::OWNED: {
            prepared_fd = fd;
            break;
          }

            // NOTE: By not setting a default we leverage the compiler
            // errors when the enumeration is augmented to find all
            // the cases we need to provide. Same for below.
        }

        OutputFileDescriptors fds;
        fds.write = prepared_fd;
        return fds;
      });
}


namespace internal {

static void cleanup(
    const Future<Option<int>>& result,
    Promise<Option<int>>* promise,
    const Subprocess& subprocess)
{
  CHECK(!result.isPending());
  CHECK(!result.isDiscarded());

  if (result.isFailed()) {
    promise->fail(result.failure());
  } else {
    promise->set(result.get());
  }

  delete promise;
}


static void close(std::initializer_list<int_fd> fds)
{
  foreach (int_fd fd, fds) {
    if (fd >= 0) {
      os::close(fd);
    }
  }
}


// This function will invoke `os::close` on all specified file
// descriptors that are valid (i.e., not `None` and >= 0).
static void close(
    const Subprocess::IO::InputFileDescriptors& stdinfds,
    const Subprocess::IO::OutputFileDescriptors& stdoutfds,
    const Subprocess::IO::OutputFileDescriptors& stderrfds)
{
  close(
      {stdinfds.read,
       stdinfds.write.getOrElse(-1),
       stdoutfds.read.getOrElse(-1),
       stdoutfds.write,
       stderrfds.read.getOrElse(-1),
       stderrfds.write});
}

}  // namespace internal {


// Executes a subprocess.
//
// NOTE: On Windows, components of the `path` and `argv` that need to be quoted
// are expected to have been quoted before they are passed to `subprocess. For
// example, either of these may contain paths with spaces in them, like
// `C:\"Program Files"\foo.exe`, where notably the character sequence `\"`
// is not escaped quote, but instead a path separator and the start of a path
// component. Since the semantics of quoting are shell-dependent, it is not
// practical to attempt to re-parse the command that is passed in and properly
// escape it. Therefore, incorrectly-quoted command arguments will probably
// lead the child process to terminate with an error.
Try<Subprocess> subprocess(
    const string& path,
    vector<string> argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& _clone,
    const vector<Subprocess::ParentHook>& parent_hooks,
    const vector<Subprocess::ChildHook>& child_hooks,
    const vector<int_fd>& whitelist_fds)
{
  // TODO(hausdorff): We should error out on Windows here if we are passing
  // parameters that aren't used.

  // File descriptors for redirecting stdin/stdout/stderr.
  // These file descriptors are used for different purposes depending
  // on the specified I/O modes.
  // See `Subprocess::PIPE`, `Subprocess::PATH`, and `Subprocess::FD`.
  InputFileDescriptors stdinfds;
  OutputFileDescriptors stdoutfds;
  OutputFileDescriptors stderrfds;

  // Prepare the file descriptor(s) for stdin.
  Try<InputFileDescriptors> input = in.input();
  if (input.isError()) {
    return Error(input.error());
  }

  stdinfds = input.get();

  // Prepare the file descriptor(s) for stdout.
  Try<OutputFileDescriptors> output = out.output();
  if (output.isError()) {
    process::internal::close(stdinfds, stdoutfds, stderrfds);
    return Error(output.error());
  }

  stdoutfds = output.get();

  // Prepare the file descriptor(s) for stderr.
  output = err.output();
  if (output.isError()) {
    process::internal::close(stdinfds, stdoutfds, stderrfds);
    return Error(output.error());
  }

  stderrfds = output.get();

#ifndef __WINDOWS__
  // TODO(jieyu): Consider using O_CLOEXEC for atomic close-on-exec.
  Try<Nothing> cloexec = internal::cloexec(stdinfds, stdoutfds, stderrfds);
  if (cloexec.isError()) {
    process::internal::close(stdinfds, stdoutfds, stderrfds);
    return Error("Failed to cloexec: " + cloexec.error());
  }
#endif // __WINDOWS__

  // Prepare the arguments. If the user specifies the 'flags', we will
  // stringify them and append them to the existing arguments.
  if (flags != nullptr) {
    foreachvalue (const flags::Flag& flag, *flags) {
      Option<string> value = flag.stringify(*flags);
      if (value.isSome()) {
        argv.push_back("--" + flag.effective_name().value + "=" + value.get());
      }
    }
  }

  // Parent.
  Subprocess process;

  // Create child, passing in stdin/stdout/stderr handles. We store the
  // information and data structures we need to manage the lifecycle of the
  // child (e.g., the PID of the child) in `process.data`.
  //
  // NOTE: We use lexical blocking around the `#ifdef` to limit use of data
  // structures declared in the `#ifdef`. Since objects like `pid` will go out
  // of scope at the end of the block, there is no chance we will accidentally
  // define something in (say) the `__WINDOWS__` branch and then attempt to use
  // it in the non-`__WINDOWS__` branch.
  {
#ifndef __WINDOWS__
    Try<pid_t> pid = internal::cloneChild(
        path,
        argv,
        environment,
        _clone,
        parent_hooks,
        child_hooks,
        stdinfds,
        stdoutfds,
        stderrfds,
        whitelist_fds);

    if (pid.isError()) {
      return Error(pid.error());
    }

    process.data->pid = pid.get();
#else
    // TODO(joerg84): Consider using the childHooks and parentHooks here.
    Try<os::windows::internal::ProcessData> process_data =
      internal::createChildProcess(
          path,
          argv,
          environment,
          parent_hooks,
          stdinfds,
          stdoutfds,
          stderrfds,
          whitelist_fds);

    if (process_data.isError()) {
      // NOTE: `createChildProcess` either succeeds entirely or returns an
      // `Error`. There is no need to check the value of the PID.
      return Error(process_data.error());
    }

    // NOTE: We specifically tie the lifetime of the child process to
    // the `Subprocess` object by saving the handles here so that
    // there is zero chance of the `pid` getting reused.
    process.data->process_data = process_data.get();
    process.data->pid = process_data->pid;
#endif // __WINDOWS__
  }

  // For any pipes, store the parent side of the pipe so that
  // the user can communicate with the subprocess.
  process.data->in = stdinfds.write;
  process.data->out = stdoutfds.read;
  process.data->err = stderrfds.read;

  // Rather than directly exposing the future from process::reap, we
  // must use an explicit promise so that we can ensure we can receive
  // the termination signal. Otherwise, the caller can discard the
  // reap future, and we will not know when it is safe to close the
  // file descriptors.
  Promise<Option<int>>* promise = new Promise<Option<int>>();
  process.data->status = promise->future();

  // We need to bind a copy of this Subprocess into the onAny callback
  // below to ensure that we don't close the file descriptors before
  // the subprocess has terminated (i.e., because the caller doesn't
  // keep a copy of this Subprocess around themselves).
  process::reap(process.data->pid)
    .onAny(lambda::bind(internal::cleanup, lambda::_1, promise, process));

  return process;
}

}  // namespace process {
