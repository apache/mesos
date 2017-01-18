// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_POSIX_SHELL_HPP__
#define __STOUT_OS_POSIX_SHELL_HPP__

#include <stdarg.h> // For va_list, va_start, etc.
#include <stdio.h> // For ferror, fgets, FILE, pclose, popen.

#include <sys/wait.h> // For waitpid.

#include <ostream>
#include <string>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/format.hpp>
#include <stout/try.hpp>

#include <stout/os/raw/argv.hpp>

namespace os {

namespace Shell {

// Canonical constants used as platform-dependent args to `exec`
// calls. `name` is the command name, `arg0` is the first argument
// received by the callee, usually the command name and `arg1` is the
// second command argument received by the callee.

constexpr const char* name = "sh";
constexpr const char* arg0 = "sh";
constexpr const char* arg1 = "-c";

} // namespace Shell {

/**
 * Runs a shell command with optional arguments.
 *
 * This assumes that a successful execution will result in the exit code
 * for the command to be `EXIT_SUCCESS`; in this case, the contents
 * of the `Try` will be the contents of `stdout`.
 *
 * If the exit code is non-zero or the process was signaled, we will
 * return an appropriate error message; but *not* `stderr`.
 *
 * If the caller needs to examine the contents of `stderr` it should
 * be redirected to `stdout` (using, e.g., "2>&1 || true" in the command
 * string).  The `|| true` is required to obtain a success exit
 * code in case of errors, and still obtain `stderr`, as piped to
 * `stdout`.
 *
 * @param fmt the formatting string that contains the command to execute
 *   in the underlying shell.
 * @param t optional arguments for `fmt`.
 *
 * @return the output from running the specified command with the shell; or
 *   an error message if the command's exit code is non-zero.
 */
template <typename... T>
Try<std::string> shell(const std::string& fmt, const T&... t)
{
  const Try<std::string> command = strings::internal::format(fmt, t...);
  if (command.isError()) {
    return Error(command.error());
  }

  FILE* file;
  std::ostringstream stdout;

  if ((file = popen(command.get().c_str(), "r")) == nullptr) {
    return Error("Failed to run '" + command.get() + "'");
  }

  char line[1024];
  // NOTE(vinod): Ideally the if and while loops should be interchanged. But
  // we get a broken pipe error if we don't read the output and simply close.
  while (fgets(line, sizeof(line), file) != nullptr) {
    stdout << line;
  }

  if (ferror(file) != 0) {
    pclose(file); // Ignoring result since we already have an error.
    return Error("Error reading output of '" + command.get() + "'");
  }

  int status;
  if ((status = pclose(file)) == -1) {
    return Error("Failed to get status of '" + command.get() + "'");
  }

  if (WIFSIGNALED(status)) {
    return Error(
        "Running '" + command.get() + "' was interrupted by signal '" +
        strsignal(WTERMSIG(status)) + "'");
  } else if ((WEXITSTATUS(status) != EXIT_SUCCESS)) {
    LOG(ERROR) << "Command '" << command.get()
               << "' failed; this is the output:\n" << stdout.str();
    return Error(
        "Failed to execute '" + command.get() + "'; the command was either "
        "not found or exited with a non-zero exit status: " +
        stringify(WEXITSTATUS(status)));
  }

  return stdout.str();
}


// Executes a command by calling "/bin/sh -c <command>", and returns
// after the command has been completed. Returns 0 if succeeds, and
// return -1 on error (e.g., fork/exec/waitpid failed). This function
// is async signal safe. We return int instead of returning a Try
// because Try involves 'new', which is not async signal safe.
//
// Note: Be cautious about shell injection
// (https://en.wikipedia.org/wiki/Code_injection#Shell_injection)
// when using this method and use proper validation and sanitization
// on the `command`. For this reason in general `os::spawn` is
// preferred if a shell is not required.
inline int system(const std::string& command)
{
  pid_t pid = ::fork();

  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // In child process.
    ::execlp(
        Shell::name, Shell::arg0, Shell::arg1, command.c_str(), (char*)nullptr);
    ::exit(127);
  } else {
    // In parent process.
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
      if (errno != EINTR) {
        return -1;
      }
    }

    return status;
  }
}

// Executes a command by calling "<command> <arguments...>", and
// returns after the command has been completed. Returns 0 if
// succeeds, and -1 on error (e.g., fork/exec/waitpid failed). This
// function is async signal safe. We return int instead of returning a
// Try because Try involves 'new', which is not async signal safe.
inline int spawn(
    const std::string& command,
    const std::vector<std::string>& arguments)
{
  pid_t pid = ::fork();

  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // In child process.
    ::execvp(command.c_str(), os::raw::Argv(arguments));
    ::exit(127);
  } else {
    // In parent process.
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
      if (errno != EINTR) {
        return -1;
      }
    }

    return status;
  }
}


template<typename... T>
inline int execlp(const char* file, T... t)
{
  return ::execlp(file, t...);
}


inline int execvp(const char* file, char* const argv[])
{
  return ::execvp(file, argv);
}

} // namespace os {

#endif // __STOUT_OS_POSIX_SHELL_HPP__
