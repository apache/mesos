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

using std::map;
using std::string;
using std::vector;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;


Subprocess::Hook::Hook(
    const lambda::function<Try<Nothing>(pid_t)>& _parent_callback)
  : parent_callback(_parent_callback) {}

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

}  // namespace internal {


// Executes a subprocess.
//
// NOTE: On Windows, components of the `path` and `argv` that need to be quoted
// are expected to have been quoted before they are passed to `subprocess. For
// example, either of these may contain paths with spaces in them, like
// `C:\"Program Files"\foo.exe`, where notably the character sequence `\"` does
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
    const Setsid set_sid,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& _clone,
    const vector<Subprocess::Hook>& parent_hooks,
    const Option<string>& working_directory,
    const Watchdog watchdog)
{
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
        set_sid,
        environment,
        _clone,
        parent_hooks,
        working_directory,
        watchdog,
        stdinfds,
        stdoutfds,
        stderrfds);

    if (pid.isError()) {
      return Error(pid.error());
    }

    process.data->pid = pid.get();
#else
    Try<PROCESS_INFORMATION> processInformation = internal::createChildProcess(
        path, argv, environment, stdinfds, stdoutfds, stderrfds);

    if (processInformation.isError()) {
      process::internal::close(stdinfds, stdoutfds, stderrfds);
      return Error(
          "Could not launch child process" + processInformation.error());
    }

    if (processInformation.get().dwProcessId == -1) {
      // Save the errno as 'close' below might overwrite it.
      ErrnoError error("Failed to clone");
      process::internal::close(stdinfds, stdoutfds, stderrfds);
      return error;
    }

    process.data->processInformation = processInformation.get();
    process.data->pid = processInformation.get().dwProcessId;
#endif // __WINDOWS__
  }

  // Close the child-ends of the file descriptors that are created
  // by this function.
  os::close(stdinfds.read);
  os::close(stdoutfds.write);
  os::close(stderrfds.write);

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
