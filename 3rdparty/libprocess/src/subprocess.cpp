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

#include <stout/os/execenv.hpp>

using std::map;
using std::string;
using std::vector;

namespace process {
namespace internal {

// See the comment below as to why subprocess is passed to cleanup.
static void cleanup(
    const Future<Option<int> >& result,
    Promise<Option<int> >* promise,
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
    return UNREACHABLE();
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
    os::ExecEnv* envp,
    const Option<lambda::function<int()> >& setup,
    int stdinFd[2],
    int stdoutFd[2],
    int stderrFd[2])
{
  // Close parent's end of the pipes.
  if (in.isPipe()) {
    while (::close(stdinFd[1]) == -1 && errno == EINTR);
  }
  if (out.isPipe()) {
    while (::close(stdoutFd[0]) == -1 && errno == EINTR);
  }
  if (err.isPipe()) {
    while (::close(stderrFd[0]) == -1 && errno == EINTR);
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
    while (::close(stdinFd[0]) == -1 && errno == EINTR);
  }
  if (stdoutFd[1] != STDIN_FILENO &&
      stdoutFd[1] != STDOUT_FILENO &&
      stdoutFd[1] != STDERR_FILENO) {
    while (::close(stdoutFd[1]) == -1 && errno == EINTR);
  }
  if (stderrFd[1] != STDIN_FILENO &&
      stderrFd[1] != STDOUT_FILENO &&
      stderrFd[1] != STDERR_FILENO) {
    while (::close(stderrFd[1]) == -1 && errno == EINTR);
  }

  if (setup.isSome()) {
    int status = setup.get()();
    if (status != 0) {
      _exit(status);
    }
  }

  execve(path.c_str(), argv, (*envp)());

  ABORT("Failed to execve in childMain\n");

  return UNREACHABLE();
}


Try<Subprocess> subprocess(
    const string& path,
    vector<string> argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const Option<flags::FlagsBase>& flags,
    const Option<map<string, string> >& environment,
    const Option<lambda::function<int()> >& setup,
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)> >& _clone)
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
      Try<int> open = os::open(in.path.get(), O_RDONLY);
      if (open.isError()) {
        return Error(
            "Failed to open '" + in.path.get() + "': " + open.error());
      }
      stdinFd[0] = open.get();
      break;
    }
    default:
      return Try<Subprocess>(UNREACHABLE());
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
          O_WRONLY | O_CREAT | O_APPEND,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

      if (open.isError()) {
        internal::close(stdinFd, stdoutFd, stderrFd);
        return Error(
            "Failed to open '" + out.path.get() + "': " + open.error());
      }
      stdoutFd[1] = open.get();
      break;
    }
    default:
      return Try<Subprocess>(UNREACHABLE());
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
          O_WRONLY | O_CREAT | O_APPEND,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

      if (open.isError()) {
        internal::close(stdinFd, stdoutFd, stderrFd);
        return Error(
            "Failed to open '" + err.path.get() + "': " + open.error());
      }
      stderrFd[1] = open.get();
      break;
    }
    default:
      return Try<Subprocess>(UNREACHABLE());
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

  // The real arguments that will be passed to 'execve'. We need to
  // construct them here before doing the clone as it might not be
  // async signal safe.
  char** _argv = new char*[argv.size() + 1];
  for (int i = 0; i < argv.size(); i++) {
    _argv[i] = (char*) argv[i].c_str();
  }
  _argv[argv.size()] = NULL;

  // We need to do this construction before doing the clone as it
  // might not be async-safe.
  // TODO(tillt): Consider optimizing this to not pass an empty map
  // into the constructor or even further to use execl instead of
  // execle once we have no user supplied environment.
  os::ExecEnv envp(environment.get(map<string, string>()));

  // Determine the function to clone the child process. If the user
  // does not specify the clone function, we will use the default.
  lambda::function<pid_t(const lambda::function<int()>&)> clone =
    (_clone.isSome() ? _clone.get() : defaultClone);

  // Now, clone the child process.
  pid_t pid = clone(lambda::bind(
      &childMain,
      path,
      _argv,
      in,
      out,
      err,
      &envp,
      setup,
      stdinFd,
      stdoutFd,
      stderrFd));

  delete[] _argv;

  if (pid == -1) {
    // Save the errno as 'close' below might overwrite it.
    ErrnoError error("Failed to clone");
    internal::close(stdinFd, stdoutFd, stderrFd);
    return error;
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
  Promise<Option<int> >* promise = new Promise<Option<int> >();
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
