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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

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
#include <stout/unreachable.hpp>

using std::map;
using std::string;
using std::vector;

namespace process {

Subprocess::Hook::Hook(
    const lambda::function<Try<Nothing>(pid_t)>& _parent_callback)
  : parent_callback(_parent_callback) {}

namespace internal {

// See the comment below as to why subprocess is passed to cleanup.
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


static void close(int stdinFd[2], int stdoutFd[2], int stderrFd[2])
{
  os::close(stdinFd[0]);
  os::close(stdinFd[1]);
  os::close(stdoutFd[0]);
  os::close(stdoutFd[1]);
  os::close(stderrFd[0]);
  os::close(stderrFd[1]);
}

// This function will invoke os::cloexec on all file descriptors in
// these pairs that are valid (i.e., >= 0).
static Try<Nothing> cloexec(int stdinFd[2], int stdoutFd[2], int stderrFd[2])
{
  int fd[6] = {
    stdinFd[0],
    stdinFd[1],
    stdoutFd[0],
    stdoutFd[1],
    stderrFd[0],
    stderrFd[1]
  };

  for (int i = 0; i < 6; i++) {
    if (fd[i] >= 0) {
      Try<Nothing> cloexec = os::cloexec(fd[i]);
      if (cloexec.isError()) {
        return Error(cloexec.error());
      }
    }
  }

  return Nothing();
}

}  // namespace internal {


static pid_t defaultClone(const lambda::function<int()>& func)
{
  pid_t pid = ::fork();
  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // Child.
    ::exit(func());
    UNREACHABLE();
  } else {
    // Parent.
    return pid;
  }
}


// The main entry of the child process. Note that this function has to
// be async singal safe.
static int childMain(
    const string& path,
    char** argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    char** envp,
    const Option<lambda::function<int()>>& setup,
    int stdinFd[2],
    int stdoutFd[2],
    int stderrFd[2],
    bool blocking,
    int pipes[2])
{
  // Close parent's end of the pipes.
  if (in.isPipe()) {
    ::close(stdinFd[1]);
  }
  if (out.isPipe()) {
    ::close(stdoutFd[0]);
  }
  if (err.isPipe()) {
    ::close(stderrFd[0]);
  }

  // Currently we will block the child's execution of the new process
  // until all the parent hooks (if any) have executed.
  if (blocking) {
    ::close(pipes[1]);
  }

  // Redirect I/O for stdin/stdout/stderr.
  while (::dup2(stdinFd[0], STDIN_FILENO) == -1 && errno == EINTR);
  while (::dup2(stdoutFd[1], STDOUT_FILENO) == -1 && errno == EINTR);
  while (::dup2(stderrFd[1], STDERR_FILENO) == -1 && errno == EINTR);

  // Close the copies. We need to make sure that we do not close the
  // file descriptor assigned to stdin/stdout/stderr in case the
  // parent has closed stdin/stdout/stderr when calling this
  // function (in that case, a dup'ed file descriptor may have the
  // same file descriptor number as stdin/stdout/stderr).
  if (stdinFd[0] != STDIN_FILENO &&
      stdinFd[0] != STDOUT_FILENO &&
      stdinFd[0] != STDERR_FILENO) {
    ::close(stdinFd[0]);
  }
  if (stdoutFd[1] != STDIN_FILENO &&
      stdoutFd[1] != STDOUT_FILENO &&
      stdoutFd[1] != STDERR_FILENO) {
    ::close(stdoutFd[1]);
  }
  if (stderrFd[1] != STDIN_FILENO &&
      stderrFd[1] != STDOUT_FILENO &&
      stderrFd[1] != STDERR_FILENO) {
    ::close(stderrFd[1]);
  }

  if (blocking) {
    // Do a blocking read on the pipe until the parent signals us to
    // continue.
    char dummy;
    ssize_t length;
    while ((length = ::read(pipes[0], &dummy, sizeof(dummy))) == -1 &&
          errno == EINTR);

    if (length != sizeof(dummy)) {
      ABORT("Failed to synchronize with parent");
    }

    // Now close the pipe as we don't need it anymore.
    ::close(pipes[0]);
  }

  if (setup.isSome()) {
    int status = setup.get()();
    if (status != 0) {
      _exit(status);
    }
  }

  os::execvpe(path.c_str(), argv, envp);

  ABORT(string("Failed to os::execvpe in childMain: ") + strerror(errno));
}


Try<Subprocess> subprocess(
    const string& path,
    vector<string> argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const Option<flags::FlagsBase>& flags,
    const Option<map<string, string>>& environment,
    const Option<lambda::function<int()>>& setup,
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& _clone,
    const std::vector<Subprocess::Hook>& parent_hooks)
{
  // File descriptors for redirecting stdin/stdout/stderr. These file
  // descriptors are used for different purposes depending on the
  // specified I/O modes. If the mode is PIPE, the two file
  // descriptors represent two ends of a pipe. If the mode is PATH or
  // FD, only one of the two file descriptors is used. Our protocol
  // here is that index 0 is always for reading, and index 1 is always
  // for writing (similar to the pipe semantics).
  int stdinFd[2] = { -1, -1 };
  int stdoutFd[2] = { -1, -1 };
  int stderrFd[2] = { -1, -1 };

  // Prepare the file descriptor(s) for stdin.
  switch (in.mode) {
    case Subprocess::IO::FD: {
      stdinFd[0] = ::dup(in.fd.get());
      if (stdinFd[0] == -1) {
        return ErrnoError("Failed to dup");
      }
      break;
    }
    case Subprocess::IO::PIPE: {
      if (::pipe(stdinFd) == -1) {
        return ErrnoError("Failed to create pipe");
      }
      break;
    }
    case Subprocess::IO::PATH: {
      Try<int> open = os::open(in.path.get(), O_RDONLY | O_CLOEXEC);
      if (open.isError()) {
        return Error(
            "Failed to open '" + in.path.get() + "': " + open.error());
      }
      stdinFd[0] = open.get();
      break;
    }
    default:
      UNREACHABLE();
  }

  // Prepare the file descriptor(s) for stdout.
  switch (out.mode) {
    case Subprocess::IO::FD: {
      stdoutFd[1] = ::dup(out.fd.get());
      if (stdoutFd[1] == -1) {
        // Save the errno as 'close' below might overwrite it.
        ErrnoError error("Failed to dup");
        internal::close(stdinFd, stdoutFd, stderrFd);
        return error;
      }
      break;
    }
    case Subprocess::IO::PIPE: {
      if (::pipe(stdoutFd) == -1) {
        // Save the errno as 'close' below might overwrite it.
        ErrnoError error("Failed to create pipe");
        internal::close(stdinFd, stdoutFd, stderrFd);
        return error;
      }
      break;
    }
    case Subprocess::IO::PATH: {
      Try<int> open = os::open(
          out.path.get(),
          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (open.isError()) {
        internal::close(stdinFd, stdoutFd, stderrFd);
        return Error(
            "Failed to open '" + out.path.get() + "': " + open.error());
      }
      stdoutFd[1] = open.get();
      break;
    }
    default:
      UNREACHABLE();
  }

  // Prepare the file descriptor(s) for stderr.
  switch (err.mode) {
    case Subprocess::IO::FD: {
      stderrFd[1] = ::dup(err.fd.get());
      if (stderrFd[1] == -1) {
        // Save the errno as 'close' below might overwrite it.
        ErrnoError error("Failed to dup");
        internal::close(stdinFd, stdoutFd, stderrFd);
        return error;
      }
      break;
    }
    case Subprocess::IO::PIPE: {
      if (::pipe(stderrFd) == -1) {
        // Save the errno as 'close' below might overwrite it.
        ErrnoError error("Failed to create pipe");
        internal::close(stdinFd, stdoutFd, stderrFd);
        return error;
      }
      break;
    }
    case Subprocess::IO::PATH: {
      Try<int> open = os::open(
          err.path.get(),
          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (open.isError()) {
        internal::close(stdinFd, stdoutFd, stderrFd);
        return Error(
            "Failed to open '" + err.path.get() + "': " + open.error());
      }
      stderrFd[1] = open.get();
      break;
    }
    default:
      UNREACHABLE();
  }

  // TODO(jieyu): Consider using O_CLOEXEC for atomic close-on-exec.
  Try<Nothing> cloexec = internal::cloexec(stdinFd, stdoutFd, stderrFd);
  if (cloexec.isError()) {
    internal::close(stdinFd, stdoutFd, stderrFd);
    return Error("Failed to cloexec: " + cloexec.error());
  }

  // Prepare the arguments. If the user specifies the 'flags', we will
  // stringify them and append them to the existing arguments.
  if (flags.isSome()) {
    foreachpair (const string& name, const flags::Flag& flag, flags.get()) {
      Option<string> value = flag.stringify(flags.get());
      if (value.isSome()) {
        argv.push_back("--" + name + "=" + value.get());
      }
    }
  }

  // The real arguments that will be passed to 'os::execvpe'. We need
  // to construct them here before doing the clone as it might not be
  // async signal safe to perform the memory allocation.
  char** _argv = new char*[argv.size() + 1];
  for (int i = 0; i < argv.size(); i++) {
    _argv[i] = (char*) argv[i].c_str();
  }
  _argv[argv.size()] = NULL;

  // Like above, we need to construct the environment that we'll pass
  // to 'os::execvpe' as it might not be async-safe to perform the
  // memory allocations.
  char** envp = os::environ();

  if (environment.isSome()) {
    // NOTE: We add 1 to the size for a NULL terminator.
    envp = new char*[environment.get().size() + 1];

    size_t index = 0;
    foreachpair (const string& key, const string& value, environment.get()) {
      string entry = key + "=" + value;
      envp[index] = new char[entry.size() + 1];
      strncpy(envp[index], entry.c_str(), entry.size() + 1);
      ++index;
    }

    envp[index] = NULL;
  }

  // Determine the function to clone the child process. If the user
  // does not specify the clone function, we will use the default.
  lambda::function<pid_t(const lambda::function<int()>&)> clone =
    (_clone.isSome() ? _clone.get() : defaultClone);

  // Currently we will block the child's execution of the new process
  // until all the `parent_hooks` (if any) have executed.
  int pipes[2];
  const bool blocking = !parent_hooks.empty();

  if (blocking) {
    // We assume this should not fail under reasonable conditions so we
    // use CHECK.
    CHECK_EQ(0, ::pipe(pipes));
  }

  // Now, clone the child process.
  pid_t pid = clone(lambda::bind(
      &childMain,
      path,
      _argv,
      in,
      out,
      err,
      envp,
      setup,
      stdinFd,
      stdoutFd,
      stderrFd,
      blocking,
      pipes));

  delete[] _argv;

  // Need to delete 'envp' if we had environment variables passed to
  // us and we needed to allocate the space.
  if (environment.isSome()) {
    CHECK_NE(os::environ(), envp);
    delete[] envp;
  }

  if (pid == -1) {
    // Save the errno as 'close' below might overwrite it.
    ErrnoError error("Failed to clone");

    internal::close(stdinFd, stdoutFd, stderrFd);

    if (blocking) {
      os::close(pipes[0]);
      os::close(pipes[1]);
    }

    return error;
  }

  if (blocking) {
    os::close(pipes[0]);

    // Run the parent hooks.
    foreach (const Subprocess::Hook& hook, parent_hooks) {
      Try<Nothing> callback = hook.parent_callback(pid);

      // If the hook callback fails, we shouldn't proceed with the
      // execution.
      if (callback.isError()) {
        LOG(WARNING)
          << "Failed to execute Subprocess::Hook in parent for child '"
          << pid << "': " << callback.error();

        os::close(pipes[1]);

        // Close the child-ends of the file descriptors that are created
        // by this function.
        os::close(stdinFd[0]);
        os::close(stdoutFd[1]);
        os::close(stderrFd[1]);

        // Ensure the child is killed.
        ::kill(pid, SIGKILL);

        return Error(
            "Failed to execute Subprocess::Hook in parent for child '" +
            stringify(pid) + "': " + callback.error());
      }
    }

    // Now that we've executed the parent hooks, we can signal the child to
    // continue by writing to the pipe.
    char dummy;
    ssize_t length;
    while ((length = ::write(pipes[1], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);

    os::close(pipes[1]);

    if (length != sizeof(dummy)) {
      // Ensure the child is killed.
      ::kill(pid, SIGKILL);

      // Close the child-ends of the file descriptors that are created
      // by this function.
      os::close(stdinFd[0]);
      os::close(stdoutFd[1]);
      os::close(stderrFd[1]);
      return Error("Failed to synchronize child process");
    }
  }

  // Parent.
  Subprocess process;
  process.data->pid = pid;

  // Close the file descriptors that are created by this function. For
  // pipes, we close the child ends and store the parent ends (see the
  // code below).
  os::close(stdinFd[0]);
  os::close(stdoutFd[1]);
  os::close(stderrFd[1]);

  // If the mode is PIPE, store the parent side of the pipe so that
  // the user can communicate with the subprocess.
  if (in.mode == Subprocess::IO::PIPE) {
    process.data->in = stdinFd[1];
  }
  if (out.mode == Subprocess::IO::PIPE) {
    process.data->out = stdoutFd[0];
  }
  if (err.mode == Subprocess::IO::PIPE) {
    process.data->err = stderrFd[0];
  }

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
