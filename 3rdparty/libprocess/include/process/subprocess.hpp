#ifndef __PROCESS_SUBPROCESS_HPP__
#define __PROCESS_SUBPROCESS_HPP__

#include <sys/types.h>

#include <map>
#include <string>

#include <process/future.hpp>

#include <stout/lambda.hpp>
#include <stout/memory.hpp>
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
  friend Try<Subprocess> subprocess(
      const std::string& command,
      const Option<std::map<std::string, std::string> >& environment,
      const Option<lambda::function<int()> >& setup);

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
    const std::string& command,
    const Option<std::map<std::string, std::string> >& environment = None(),
    const Option<lambda::function<int()> >& setup = None());

} // namespace process {

#endif // __PROCESS_SUBPROCESS_HPP__
