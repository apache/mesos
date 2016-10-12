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

#ifndef __STOUT_OS_WINDOWS_SHELL_HPP__
#define __STOUT_OS_WINDOWS_SHELL_HPP__

#include <process.h>
#include <stdarg.h> // For va_list, va_start, etc.

#include <ostream>
#include <string>

#include <stout/try.hpp>

#include <stout/os/raw/argv.hpp>

namespace os {

namespace Shell {

  // Canonical constants used as platform-dependent args to `exec` calls.
  // `name` is the command name, `arg0` is the first argument received
  // by the callee, usually the command name and `arg1` is the second
  // command argument received by the callee.
  constexpr const char* name = "cmd.exe";
  constexpr const char* arg0 = "cmd";
  constexpr const char* arg1 = "/c";

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
  std::ostringstream stdoutstr;

  if ((file = _popen(command.get().c_str(), "r")) == nullptr) {
    return Error("Failed to run '" + command.get() + "'");
  }

  char line[1024];
  // NOTE(vinod): Ideally the if and while loops should be interchanged. But
  // we get a broken pipe error if we don't read the output and simply close.
  while (fgets(line, sizeof(line), file) != nullptr) {
    stdoutstr << line;
  }

  if (ferror(file) != 0) {
    _pclose(file); // Ignoring result since we already have an error.
    return Error("Error reading output of '" + command.get() + "'");
  }

  int status;
  if ((status = _pclose(file)) == -1) {
    return Error("Failed to get status of '" + command.get() + "'");
  }

  return stdoutstr.str();
}


// Executes a command by calling "cmd /c <command>", and returns
// after the command has been completed. Returns 0 if succeeds, and
// return -1 on error.
//
// The returned value from `_spawnlp` represents child exit code when
// `_P_WAIT` is used.
inline int system(const std::string& command)
{
  return static_cast<int>(::_spawnlp(
    _P_WAIT, Shell::name, Shell::arg0, Shell::arg1, command.c_str(), nullptr));
}


// Executes a command by calling "<command> <arguments...>", and
// returns after the command has been completed. Returns 0 if
// succeeds, and -1 on error.
inline int spawn(
    const std::string& command,
    const std::vector<std::string>& arguments)
{
  return ::_spawnvp(_P_WAIT, command.c_str(), os::raw::Argv(arguments));
}


// On Windows, the `_spawnlp` call creates a new process.
// In order to emulate the semantics of `execlp`, we spawn with `_P_WAIT`,
// which forces the parent process to block on the child. When the child exits,
// the exit code is propagated back through the parent via `exit()`.
//
// The returned value from `_spawnlp` represents child exit code when
// `_P_WAIT` is used.
template<typename... T>
inline int execlp(const char* file, T... t)
{
  exit(static_cast<int>(::_spawnlp(_P_WAIT, file, t...)));
  return 0;
}


// On Windows, the `_spawnvp` call creates a new process.
// In order to emulate the semantics of `execvp`, we spawn with `_P_WAIT`,
// which forces the parent process to block on the child. When the child exits,
// the exit code is propagated back through the parent via `exit()`.
//
// The returned value from `_spawnlp` represents child exit code when
// `_P_WAIT` is used.
inline int execvp(const char* file, char* const argv[])
{
  exit(static_cast<int>(::_spawnvp(_P_WAIT, file, argv)));
  return 0;
}


// On Windows, the `_spawnvpe` call creates a new process.
// In order to emulate the semantics of `execvpe`, we spawn with `_P_WAIT`,
// which forces the parent process to block on the child. When the child exits,
// the exit code is propagated back through the parent via `exit()`.
//
// The returned value from `_spawnvpe` represents child exit code when
// `_P_WAIT` is used.
inline int execvpe(const char* file, char* const argv[], char* const envp[])
{
  exit(static_cast<int>(::_spawnvpe(_P_WAIT, file, argv, envp)));
  return 0;
}


// Concatenates multiple command-line arguments and escapes the values.
// If `arg` is not specified (or takes the value `0`), the function will
// scan `argv` until a `nullptr` is encountered.
inline std::string stringify_args(char** argv, unsigned long argc = 0)
{
  std::string arg_line = "";
  unsigned long index = 0;
  while ((argc == 0 || index < argc) && argv[index] != nullptr) {
    // TODO(dpravat): (MESOS-5522) Format these args for all cases.
    // Specifically, we need to:
    //   (1) Add double quotes around arguments that contain special
    //       characters, like spaces and tabs.
    //   (2) Escape any existing double quotes and backslashes.
    arg_line = strings::join(" ", arg_line, argv[index++]);
  }

  return arg_line;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SHELL_HPP__
