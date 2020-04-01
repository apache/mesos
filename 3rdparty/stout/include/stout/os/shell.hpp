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

#ifndef __STOUT_OS_SHELL_HPP__
#define __STOUT_OS_SHELL_HPP__

// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/os/windows/shell.hpp>
#else
#include <stout/os/posix/shell.hpp>
#endif // __WINDOWS__

namespace os {

// Executes a shell command (formatted in an sprintf manner) in a subprocess.
//
// Blocks until the subprocess terminates and returns the stdout
// (but not stderr!) from running the specified command with the
// shell; or an error message if the command's exit code is non-zero
// or the process exited from a signal.
//
// POSIX: Performs a `popen(command.c_str(), "r")` and reads the
// output until EOF or reading fails. Note that this is not an
// async signal safe function.
//
// Windows: Forks and executes a "cmd /c <command>" subprocess.
// TODO(bmahler): Rather than passing the command string to cmd /c,
// this function is incorrectly quoting / escaping it as if it's
// executing a program compatible with `CommandLineToArgvW`. cmd.exe
// Does not use `CommandLineToArgvW` and has its own parsing, which
// means we should not be adding extra quoting and the caller should
// make sure that the command is correctly escaped. See MESOS-10093.
//
// Note: Be cautious about shell injection
// (https://en.wikipedia.org/wiki/Code_injection#Shell_injection)
// when using this method and use proper validation and sanitization
// on the `command`. For this reason in general directly executing the
// program using `os::spawn` is preferred if a shell is not required.
//
// TODO(bmahler): Embedding string formatting leads to a confusing
// interface, why don't we let callers decide how they want to format
// the command string?
template <typename... T>
Try<std::string> shell(const std::string& fmt, const T&... t);


// Executes a shell command in a subprocess.
//
// Blocks until the subprocess terminates and returns the exit code of
// the subprocess, or `None` if an error occurred (e.g., fork / exec /
// waitpid or the Windows equivalents failed).
//
// POSIX: Performs a `fork()` / `execlp("sh", "-c", command, (char*)0)`
// in an async signal safe manner. Note that this is not a pure wrapper
// of the POSIX `system` call because the POSIX implementation ignores
// SIGINT/SIGQUIT and blocks SIGCHLD while waiting for the child to
// complete (which is not async-signal-safe).
//
// Windows: Forks and executes a "cmd /c <command>" subprocess.
// TODO(bmahler): Rather than passing the command string to cmd /c,
// this function is incorrectly quoting / escaping it as if it's
// executing a program compatible with `CommandLineToArgvW`. cmd.exe
// Does not use `CommandLineToArgvW` and has its own parsing, which
// means we should not be adding extra quoting and the caller should
// make sure that the command is correctly escaped. See MESOS-10093.
//
// Note: Be cautious about shell injection
// (https://en.wikipedia.org/wiki/Code_injection#Shell_injection)
// when using this method and use proper validation and sanitization
// on the `command`. For this reason in general directly executing the
// program using `os::spawn` is preferred if a shell is not required.
//
// Note that the return type is `Option<int>` rather than `Try<int>`
// because `Error` uses an std::string which is not async signal safe.
inline Option<int> system(const std::string& command);

} // namespace os {

#endif // __STOUT_OS_SHELL_HPP__
