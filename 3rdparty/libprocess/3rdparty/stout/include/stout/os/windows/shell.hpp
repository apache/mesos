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

namespace os {

// Runs a shell command formatted with varargs and return the return value
// of the command. Optionally, the output is returned via an argument.
// TODO(vinod): Pass an istream object that can provide input to the command.
template <typename... T>
Try<std::string> shell(const std::string& fmt, const T&... t)
{
  UNIMPLEMENTED;
}

// Canonical constants used as platform-dependent args to `exec` calls.
// name() is the command name, arg0() is the first argument received
// by the callee, usualy the command name and arg1() is the second
// command argument received by the callee.
struct Shell
{
  static constexpr const char* name = "cmd.exe";
  static constexpr const char* arg0 = "cmd.exe";
  static constexpr const char* arg1 = "/c";
};

// Executes a command by calling "cmd /c <command>", and returns
// after the command has been completed. Returns 0 if succeeds, and
// return -1 on error
inline int system(const std::string& command)
{
  return ::_spawnl(
      _P_WAIT, Shell::name, Shell::arg0, Shell::arg1, command.c_str());
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SHELL_HPP__
