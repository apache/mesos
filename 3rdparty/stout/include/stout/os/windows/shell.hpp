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
#include <processthreadsapi.h>
#include <shellapi.h>
#include <synchapi.h>

#include <stdarg.h> // For va_list, va_start, etc.

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/pipe.hpp>

#include <stout/os/windows/exec.hpp>

namespace os {
namespace Shell {

// Canonical constants used as platform-dependent args to `exec` calls.
// `name` is the command name, `arg0` is the first argument received
// by the callee, usually the command name and `arg1` is the second
// command argument received by the callee.
constexpr const char* name = "cmd.exe";
constexpr const char* arg0 = "cmd.exe";
constexpr const char* arg1 = "/c";

} // namespace Shell {


template <typename... T>
Try<std::string> shell(const std::string& fmt, const T&... t)
{
  using std::array;
  using std::string;
  using std::vector;

  const Try<string> command = strings::format(fmt, t...);
  if (command.isError()) {
    return Error(command.error());
  }

  // This function is intended to pass the arguments to the default
  // shell, so we first add the arguments `cmd.exe cmd.exe /c`,
  // followed by the command and arguments given.
  vector<string> args = {os::Shell::name, os::Shell::arg0, os::Shell::arg1};

  { // Minimize the lifetime of the system allocated buffer.
    //
    // NOTE: This API returns a pointer to an array of `wchar_t*`,
    // similar to `argv`. Each pointer to a null-terminated Unicode
    // string represents an individual argument found on the command
    // line. We use this because we cannot just split on whitespace.
    int argc;
    const std::unique_ptr<wchar_t*, decltype(&::LocalFree)> argv(
      ::CommandLineToArgvW(wide_stringify(command.get()).data(), &argc),
      &::LocalFree);
    if (argv == nullptr) {
      return WindowsError();
    }

    for (int i = 0; i < argc; ++i) {
      args.push_back(stringify(std::wstring(argv.get()[i])));
    }
  }

  // This function is intended to return only the `stdout` of the
  // command; but since we have to redirect all of `stdin`, `stdout`,
  // `stderr` if we want to redirect any one of them, we redirect
  // `stdin` and `stderr` to `NUL`, and `stdout` to a pipe.
  Try<int_fd> stdin_ = os::open(os::DEV_NULL, O_RDONLY);
  if (stdin_.isError()) {
    return Error(stdin_.error());
  }

  Try<array<int_fd, 2>> stdout_ = os::pipe();
  if (stdout_.isError()) {
    return Error(stdout_.error());
  }

  Try<int_fd> stderr_ = os::open(os::DEV_NULL, O_WRONLY);
  if (stderr_.isError()) {
    return Error(stderr_.error());
  }

  // Ensure the file descriptors are closed when we leave this scope.
  struct Closer
  {
    vector<int_fd> fds;
    ~Closer()
    {
      foreach (int_fd& fd, fds) {
        os::close(fd);
      }
    }
  } closer = {{stdin_.get(), stdout_.get()[0], stderr_.get()}};

  array<int_fd, 3> pipes = {stdin_.get(), stdout_.get()[1], stderr_.get()};

  using namespace os::windows::internal;

  Try<ProcessData> process_data =
    create_process(args.front(), args, None(), false, pipes);

  if (process_data.isError()) {
    return Error(process_data.error());
  }

  // Close the child end of the stdout pipe and then read until EOF.
  os::close(stdout_.get()[1]);
  string out;
  Result<string> part = None();
  do {
    part = os::read(stdout_.get()[0], 1024);
    if (part.isSome()) {
      out += part.get();
    }
  } while (part.isSome());

  // Wait for the process synchronously.
  ::WaitForSingleObject(process_data->process_handle.get_handle(), INFINITE);

  DWORD status;
  if (!::GetExitCodeProcess(
        process_data->process_handle.get_handle(), &status)) {
    return Error("Failed to `GetExitCodeProcess`: " + command.get());
  }

  if (status == 0) {
    return out;
  }

  return Error(
    "Failed to execute '" + command.get() +
    "'; the command was either "
    "not found or exited with a non-zero exit status: " +
    stringify(status));
}


inline Option<int> system(const std::string& command)
{
  return os::spawn(Shell::name, {Shell::arg0, Shell::arg1, command}, None());
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SHELL_HPP__
