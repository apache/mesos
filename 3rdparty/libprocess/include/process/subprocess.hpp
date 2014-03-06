#ifndef __PROCESS_SUBPROCESS_HPP__
#define __PROCESS_SUBPROCESS_HPP__

#include <unistd.h>

#include <glog/logging.h>

#include <sys/types.h>

#include <string>

#include <process/future.hpp>
#include <process/reap.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace process {

// Represents a fork() exec()ed subprocess. Access is provided to
// the input / output of the process, as well as the exit status.
// The input / output file descriptors are only closed after both:
//   1. The subprocess has terminated, and
//   2. There are no longer any references to the associated
//      Subprocess object.
struct Subprocess
{
  // Returns the pid for the subprocess.
  pid_t pid() const { return data->pid; }

  // File descriptor accessors for input / output.
  int in()  const { return data->in;  }
  int out() const { return data->out; }
  int err() const { return data->err; }

  // Returns a future from process::reap of this subprocess.
  // Discarding this future has no effect on the subprocess.
  Future<Option<int> > status() const { return data->status; }

private:
  Subprocess() : data(new Data()) {}
  friend Try<Subprocess> subprocess(const std::string&);

  struct Data
  {
    ~Data()
    {
      os::close(in);
      os::close(out);
      os::close(err);
    }

    pid_t pid;

    // NOTE: stdin, stdout, stderr are macros on some systems, hence
    // these names instead.
    int in;
    int out;
    int err;

    Future<Option<int> > status;
  };

  memory::shared_ptr<Data> data;
};


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

}


// Runs the provided command in a subprocess.
inline Try<Subprocess> subprocess(const std::string& command)
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

    execl("/bin/sh", "sh", "-c", command.c_str(), (char *) NULL);

    ABORT("Failed to execl '/bin sh -c ", command.c_str(), "'\n");
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

} // namespace process {

#endif // __PROCESS_SUBPROCESS_HPP__
