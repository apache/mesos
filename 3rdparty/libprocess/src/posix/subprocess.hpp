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

#ifndef __PROCESS_POSIX_SUBPROCESS_HPP__
#define __PROCESS_POSIX_SUBPROCESS_HPP__

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif // __linux__
#include <sys/types.h>

#include <string>

#include <glog/logging.h>

#include <process/subprocess.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/close.hpp>
#include <stout/os/environment.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/signals.hpp>
#include <stout/os/strerror.hpp>

namespace process {
namespace internal {

static void close(std::initializer_list<int_fd> fds);


static void close(
    const Subprocess::IO::InputFileDescriptors& stdinfds,
    const Subprocess::IO::OutputFileDescriptors& stdoutfds,
    const Subprocess::IO::OutputFileDescriptors& stderrfds);


#if defined(__linux__) && defined(SYS_getdents64)
// Convert a null-terminated string to an integer. This function
// is async signal safe since it does not make any libc calls.
static int convertStringToInt(const char *name)
{
  int num = 0;
  while (*name >= '0' && *name <= '9') {
    num = num * 10 + (*name - '0');
    ++name;
  }

  if (*name) {
    // Non digit found, not a number.
    return -1;
  }

  return num;
}
#endif // __linux__ && SYS_getdents64


// Close any file descriptors that are not stdio file descriptors and not
// explicitly whitelisted to avoid leaking them into the forked process.
// And unset the `close-on-exec` flag for the whitelist file descriptors
// so that they can be inherited by the forked process.
static void handleWhitelistFds(const std::vector<int_fd>& whitelist_fds)
{
  // We need to make a syscall (e.g., `SYS_getdents64` on Linux) to get each
  // entry from `/dev/fd` since syscall function is async signal safe, but we
  // cannot do that for macOS since Apple has decided to deprecate all syscall
  // functions with OS 10.12 (see MESOS-8457).
#if defined(__linux__) && defined(SYS_getdents64)
  int fdDir = ::open("/dev/fd", O_RDONLY);
  if (fdDir == -1) {
    ABORT("Failed to open /dev/fd: " + os::strerror(errno));
  }

  struct linux_dirent64 {
     ino64_t        d_ino;
     off64_t        d_off;
     unsigned short d_reclen;
     unsigned char  d_type;
     char           d_name[];
  };

  char buffer[1024];
  int bytes;

  while (true) {
    bytes = ::syscall(SYS_getdents64, fdDir, buffer, sizeof(buffer));
    if (bytes == -1) {
      ABORT("Failed to call SYS_getdents64 on /dev/fd: " + os::strerror(errno));
    }

    if (bytes == 0) {
      break;
    }

    struct linux_dirent64 *entry;
    for (int offset = 0; offset < bytes; offset += entry->d_reclen) {
      entry = reinterpret_cast<struct linux_dirent64 *>(buffer + offset);
      int_fd fd = convertStringToInt(entry->d_name);
      if (fd >= 0 &&
          fd != fdDir &&
          fd != STDIN_FILENO &&
          fd != STDOUT_FILENO &&
          fd != STDERR_FILENO) {
        bool found = false;
        foreach (int_fd whitelist_fd, whitelist_fds) {
          if (whitelist_fd == fd) {
            found = true;
            break;
          }
        }

        if (!found) {
          int flags = ::fcntl(fd, F_GETFD);
          if (flags == -1) {
            // TODO(gilbert): clean up the use of `os::strerror` during the
            // timeframe of fork-exec because it is not signal safe.
            ABORT(
                "Failed to get file descriptor flags: " + os::strerror(errno));
          }

          // Close the FD which does not have the FD_CLOEXEC bit.
          if ((flags & FD_CLOEXEC) == 0){
            ::close(fd);
          }
        }
      }
    }
  }

  ::close(fdDir);
#endif // __linux__ && SYS_getdents64

  foreach (int_fd fd, whitelist_fds) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1) {
      ABORT("Failed to get file descriptor flags: " + os::strerror(errno));
    }

    if (::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == -1) {
      ABORT("Failed to unset cloexec: " + os::strerror(errno));
    }
  }
}


inline pid_t defaultClone(const lambda::function<int()>& func)
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


// This function will invoke `os::cloexec` on all specified file
// descriptors that are valid (i.e., not `None` and >= 0).
inline Try<Nothing> cloexec(
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds)
{
  hashset<int> fds = {
    stdinfds.read,
    stdinfds.write.getOrElse(-1),
    stdoutfds.read.getOrElse(-1),
    stdoutfds.write,
    stderrfds.read.getOrElse(-1),
    stderrfds.write
  };

  foreach (int fd, fds) {
    if (fd >= 0) {
      Try<Nothing> cloexec = os::cloexec(fd);
      if (cloexec.isError()) {
        return Error(cloexec.error());
      }
    }
  }

  return Nothing();
}


// The main entry of the child process.
//
// NOTE: This function has to be async signal safe.
inline int childMain(
    const std::string& path,
    char** argv,
    char** envp,
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds,
    const std::vector<int_fd>& whitelist_fds,
    bool blocking,
    int pipes[2],
    const std::vector<Subprocess::ChildHook>& child_hooks)
{
  // Close parent's end of the pipes.
  if (stdinfds.write.isSome()) {
    ::close(stdinfds.write.get());
  }
  if (stdoutfds.read.isSome()) {
    ::close(stdoutfds.read.get());
  }
  if (stderrfds.read.isSome()) {
    ::close(stderrfds.read.get());
  }

  // Currently we will block the child's execution of the new process
  // until all the parent hooks (if any) have executed.
  if (blocking) {
    ::close(pipes[1]);
  }

  // Redirect I/O for stdin/stdout/stderr.
  while (::dup2(stdinfds.read, STDIN_FILENO) == -1 && errno == EINTR);
  while (::dup2(stdoutfds.write, STDOUT_FILENO) == -1 && errno == EINTR);
  while (::dup2(stderrfds.write, STDERR_FILENO) == -1 && errno == EINTR);

  // Close the copies. We need to make sure that we do not close the
  // file descriptor assigned to stdin/stdout/stderr in case the
  // parent has closed stdin/stdout/stderr when calling this
  // function (in that case, a dup'ed file descriptor may have the
  // same file descriptor number as stdin/stdout/stderr).
  //
  // We also need to ensure that we don't "double close" any file
  // descriptors in the case where one of stdinfds.read,
  // stdoutfds.write, or stdoutfds.write are equal.
  if (stdinfds.read != STDIN_FILENO &&
      stdinfds.read != STDOUT_FILENO &&
      stdinfds.read != STDERR_FILENO) {
    ::close(stdinfds.read);
  }
  if (stdoutfds.write != STDIN_FILENO &&
      stdoutfds.write != STDOUT_FILENO &&
      stdoutfds.write != STDERR_FILENO &&
      stdoutfds.write != stdinfds.read) {
    ::close(stdoutfds.write);
  }
  if (stderrfds.write != STDIN_FILENO &&
      stderrfds.write != STDOUT_FILENO &&
      stderrfds.write != STDERR_FILENO &&
      stderrfds.write != stdinfds.read &&
      stderrfds.write != stdoutfds.write) {
    ::close(stderrfds.write);
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

  // Run the child hooks.
  foreach (const Subprocess::ChildHook& hook, child_hooks) {
    Try<Nothing> callback = hook();

    // If the callback failed, we should abort execution.
    if (callback.isError()) {
      ABORT("Failed to execute Subprocess::ChildHook: " + callback.error());
    }
  }

  handleWhitelistFds(whitelist_fds);

  os::execvpe(path.c_str(), argv, envp);

  SAFE_EXIT(
      errno, "Failed to os::execvpe on path '%s': %d", path.c_str(), errno);
}


inline Try<pid_t> cloneChild(
    const std::string& path,
    std::vector<std::string> argv,
    const Option<std::map<std::string, std::string>>& environment,
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& _clone,
    const std::vector<Subprocess::ParentHook>& parent_hooks,
    const std::vector<Subprocess::ChildHook>& child_hooks,
    const InputFileDescriptors stdinfds,
    const OutputFileDescriptors stdoutfds,
    const OutputFileDescriptors stderrfds,
    const std::vector<int_fd>& whitelist_fds)
{
  // The real arguments that will be passed to 'os::execvpe'. We need
  // to construct them here before doing the clone as it might not be
  // async signal safe to perform the memory allocation.
  char** _argv = new char*[argv.size() + 1];
  for (size_t i = 0; i < argv.size(); i++) {
    _argv[i] = (char*) argv[i].c_str();
  }
  _argv[argv.size()] = nullptr;

  // Like above, we need to construct the environment that we'll pass
  // to 'os::execvpe' as it might not be async-safe to perform the
  // memory allocations.
  char** envp = os::raw::environment();

  if (environment.isSome()) {
    // NOTE: We add 1 to the size for a `nullptr` terminator.
    envp = new char*[environment->size() + 1];

    size_t index = 0;
    foreachpair (
        const std::string& key,
        const std::string& value, environment.get()) {
      std::string entry = key + "=" + value;
      envp[index] = new char[entry.size() + 1];
      strncpy(envp[index], entry.c_str(), entry.size() + 1);
      ++index;
    }

    envp[index] = nullptr;
  }

  // Determine the function to clone the child process. If the user
  // does not specify the clone function, we will use the default.
  lambda::function<pid_t(const lambda::function<int()>&)> clone =
    (_clone.isSome() ? _clone.get() : defaultClone);

  // Currently we will block the child's execution of the new process
  // until all the `parent_hooks` (if any) have executed.
  std::array<int, 2> pipes;
  const bool blocking = !parent_hooks.empty();

  if (blocking) {
    // We assume this should not fail under reasonable conditions so we
    // use CHECK.
    Try<std::array<int, 2>> pipe = os::pipe();
    CHECK_SOME(pipe);

    pipes = pipe.get();
  }

  // Now, clone the child process.
  pid_t pid = clone(lambda::bind(
      &childMain,
      path,
      _argv,
      envp,
      stdinfds,
      stdoutfds,
      stderrfds,
      whitelist_fds,
      blocking,
      pipes.data(),
      child_hooks));

  delete[] _argv;

  // Need to delete 'envp' if we had environment variables passed to
  // us and we needed to allocate the space.
  if (environment.isSome()) {
    CHECK_NE(os::raw::environment(), envp);

    // We ignore the last 'envp' entry since it is nullptr.
    for (size_t index = 0; index < environment->size(); index++) {
      delete[] envp[index];
    }

    delete[] envp;
  }

  if (pid == -1) {
    // Save the errno as 'close' below might overwrite it.
    ErrnoError error("Failed to clone");
    internal::close(stdinfds, stdoutfds, stderrfds);

    if (blocking) {
      os::close(pipes[0]);
      os::close(pipes[1]);
    }

    return error;
  }

  // Close the child-ends of the file descriptors that are created by
  // this function.
  internal::close({stdinfds.read, stdoutfds.write, stderrfds.write});

  if (blocking) {
    os::close(pipes[0]);

    // Run the parent hooks.
    foreach (const Subprocess::ParentHook& hook, parent_hooks) {
      Try<Nothing> parentSetup = hook.parent_setup(pid);

      // If the hook callback fails, we shouldn't proceed with the
      // execution and hence the child process should be killed.
      if (parentSetup.isError()) {
        LOG(WARNING)
          << "Failed to execute Subprocess::ParentHook in parent for child '"
          << pid << "': " << parentSetup.error();

        os::close(pipes[1]);

        // Ensure the child is killed.
        ::kill(pid, SIGKILL);

        return Error(
            "Failed to execute Subprocess::ParentHook in parent for child '" +
            stringify(pid) + "': " + parentSetup.error());
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

      return Error("Failed to synchronize child process");
    }
  }

  return pid;
}

}  // namespace internal {
}  // namespace process {

#endif // __PROCESS_POSIX_SUBPROCESS_HPP__
