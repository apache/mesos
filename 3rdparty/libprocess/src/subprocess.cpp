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
#include <stout/try.hpp>

#include <stout/os/execenv.hpp>

using std::map;
using std::string;

namespace process {
namespace internal {

// See the comment below as to why subprocess is passed to cleanup.
void cleanup(
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

}  // namespace internal {


// Runs the provided command in a subprocess.
Try<Subprocess> subprocess(
    const string& command,
    const Option<map<string, string> >& environment,
    const Option<lambda::function<int()> >& setup)
{
  // Create pipes for stdin, stdout, stderr.
  // Index 0 is for reading, and index 1 is for writing.
  int stdinPipe[2];
  int stdoutPipe[2];
  int stderrPipe[2];

  if (pipe(stdinPipe) == -1) {
    return ErrnoError("Failed to create pipe");
  } else if (pipe(stdoutPipe) == -1) {
    os::close(stdinPipe[0]);
    os::close(stdinPipe[1]);
    return ErrnoError("Failed to create pipe");
  } else if (pipe(stderrPipe) == -1) {
    os::close(stdinPipe[0]);
    os::close(stdinPipe[1]);
    os::close(stdoutPipe[0]);
    os::close(stdoutPipe[1]);
    return ErrnoError("Failed to create pipe");
  }

  // We need to do this construction before doing the fork as it
  // might not be async-safe.
  // TODO(tillt): Consider optimizing this to not pass an empty map
  // into the constructor or even further to use execl instead of
  // execle once we have no user supplied environment.
  os::ExecEnv envp(environment.get(map<string, string>()));

  pid_t pid;
  if ((pid = fork()) == -1) {
    os::close(stdinPipe[0]);
    os::close(stdinPipe[1]);
    os::close(stdoutPipe[0]);
    os::close(stdoutPipe[1]);
    os::close(stderrPipe[0]);
    os::close(stderrPipe[1]);
    return ErrnoError("Failed to fork");
  }

  Subprocess process;
  process.data->pid = pid;

  if (process.data->pid == 0) {
    // Child.
    // Close parent's end of the pipes.
    os::close(stdinPipe[1]);
    os::close(stdoutPipe[0]);
    os::close(stderrPipe[0]);

    // Make our pipes look like stdin, stderr, stdout before we exec.
    while (dup2(stdinPipe[0], STDIN_FILENO)   == -1 && errno == EINTR);
    while (dup2(stdoutPipe[1], STDOUT_FILENO) == -1 && errno == EINTR);
    while (dup2(stderrPipe[1], STDERR_FILENO) == -1 && errno == EINTR);

    // Close the copies.
    os::close(stdinPipe[0]);
    os::close(stdoutPipe[1]);
    os::close(stderrPipe[1]);

    if (setup.isSome()) {
      int status = setup.get()();
      if (status != 0) {
        _exit(status);
      }
    }

    execle("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL, envp());

    ABORT("Failed to execle '/bin sh -c ", command.c_str(), "'\n");
  }

  // Parent.

  // Close the child's end of the pipes.
  os::close(stdinPipe[0]);
  os::close(stdoutPipe[1]);
  os::close(stderrPipe[1]);

  process.data->in = stdinPipe[1];
  process.data->out = stdoutPipe[0];
  process.data->err = stderrPipe[0];

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
