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

#include <algorithm>
#include <map>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/try.hpp>
#include <stout/unimplemented.hpp>

#include <stout/windows.hpp>

namespace internal {

namespace windows {

// Retrieves system environment in a `std::map`, ignoring
// the current process's environment variables.
inline Option<std::map<std::wstring, std::wstring>> get_system_env()
{
  std::map<std::wstring, std::wstring> system_env;
  wchar_t* env_entry = nullptr;

  // Get the system environment.
  // The third parameter (bool) tells the function *not* to inherit
  // variables from the current process.
  if (!::CreateEnvironmentBlock((LPVOID*)&env_entry, nullptr, FALSE)) {
    return None();
  }

  // Save the environment block in order to destroy it later.
  wchar_t* env_block = env_entry;

  while (*env_entry != L'\0') {
    // Each environment block contains the environment variables as follows:
    // Var1=Value1\0
    // Var2=Value2\0
    // Var3=Value3\0
    // ...
    // VarN=ValueN\0\0
    // The name of an environment variable cannot include an equal sign (=).

    // Construct a string from the pointer up to the first '\0',
    // e.g. "Var1=Value1\0", then split into name and value.
    std::wstring entry(env_entry);
    std::wstring::size_type separator = entry.find(L"=");
    std::wstring var_name(entry.substr(0, separator));
    std::wstring varVal(entry.substr(separator + 1));

    // Mesos variables are upper case. Convert system variables to
    // match the name provided by the scheduler in case of a collision.
    // This is safe because Windows environment variables are case insensitive.
    std::transform(
        var_name.begin(), var_name.end(), var_name.begin(), ::towupper);

    // The system environment has priority.
    system_env.insert_or_assign(var_name.data(), varVal.data());

    // Advance the pointer the length of the entry string plus the '\0'.
    env_entry += entry.length() + 1;
  }

  ::DestroyEnvironmentBlock(env_block);

  return system_env;
}


// Creates a null-terminated array of null-terminated strings that will be
// passed to `CreateProcessW` as the `lpEnvironment` argument, as described by
// MSDN[1]. This array needs to be sorted in alphabetical order, but the `map`
// already takes care of that. Note that this function explicitly handles
// UTF-16 environments, so it must be used in conjunction with the
// `CREATE_UNICODE_ENVIRONMENT` flag.
//
// NOTE: This function will add the system's environment variables into
// the returned string. These variables take precedence over the provided
// `env` and are generally necessary in order to launch things on Windows.
//
// [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682425(v=vs.85).aspx
inline Option<std::wstring> create_process_env(
    const Option<std::map<std::string, std::string>>& env)
{
  if (env.isNone() || (env.isSome() && env.get().size() == 0)) {
    return None();
  }

  Option<std::map<std::wstring, std::wstring>> system_env = get_system_env();

  // The system environment must be non-empty.
  // No subprocesses will be able to launch if the system environment is blank.
  CHECK(system_env.isSome() && system_env.get().size() > 0);

  std::map<std::wstring, std::wstring> combined_env;

  // Populate the combined environment first with the given environment
  // converted to UTF-16 for Windows.
  foreachpair (const std::string& key,
               const std::string& value,
               env.get()) {
    combined_env[wide_stringify(key)] = wide_stringify(value);
  }

  // Add the system environment variables, overwriting the previous.
  foreachpair (const std::wstring& key,
               const std::wstring& value,
               system_env.get()) {
    combined_env[key] = value;
  }

  std::wstring env_string;
  foreachpair (const std::wstring& key,
               const std::wstring& value,
               combined_env) {
    env_string += key + L'=' + value + L'\0';
  }

  // Append final null terminating character.
  env_string.push_back(L'\0');
  return env_string;
}


// Concatenates multiple command-line arguments and escapes the values.
// NOTE: This is necessary even when using Windows APIs that "appear"
// to take arguments as a list, because those APIs will themselves
// concatenate command-line arguments *without* escaping them.
//
// This function escapes arguments with the following rules:
//   1) Any argument with a space, tab, newline, vertical tab,
//      or double-quote must be surrounded in double-quotes.
//   2) Backslashes at the very end of an argument must be escaped.
//   3) Backslashes that precede a double-quote must be escaped.
//      The double-quote must also be escaped.
//
// NOTE: The below algorithm is adapted from Daniel Colascione's public domain
// algorithm for quoting command line arguments on Windows for `CreateProcess`.
//
// https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/
// NOLINT(whitespace/line_length)
inline std::wstring stringify_args(const std::vector<std::string>& argv)
{
  std::wstring command;
  for (auto argit = argv.cbegin(); argit != argv.cend(); ++argit) {
    std::wstring arg = wide_stringify(*argit);
    // Don't quote empty arguments or those without troublesome characters.
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == arg.npos) {
      command.append(arg);
    } else {
      // Beginning double quotation mark.
      command.push_back(L'"');
      for (auto it = arg.cbegin(); it != arg.cend(); ++it) {
        // Count existent backslashes in argument.
        unsigned int backslashes = 0;
        while (it != arg.cend() && *it == L'\\') {
          ++it;
          ++backslashes;
        }

        if (it == arg.cend()) {
          // Escape all backslashes, but let the terminating double quotation
          // mark we add below be interpreted as a metacharacter.
          command.append(backslashes * 2, L'\\');
          break;
        } else if (*it == L'"') {
          // Escape all backslashes and the following double quotation mark.
          command.append(backslashes * 2 + 1, L'\\');
          command.push_back(*it);
        } else {
          // Backslashes aren't special here.
          command.append(backslashes, L'\\');
          command.push_back(*it);
        }
      }

      // Terminating double quotation mark.
      command.push_back(L'"');
    }
    // Space separate arguments (but don't append at end).
    if (argit != argv.cend() - 1) {
      command.push_back(L' ');
    }
  }
  // Append final null terminating character.
  command.push_back(L'\0');
  return command;
}

struct ProcessData {
  SharedHandle process_handle;
  SharedHandle thread_handle;
  pid_t pid;
};

// Provides an interface for creating a child process on Windows.
//
// The `command` argument is given for compatibility, and is ignored. This is
// because the `CreateProcess` will use `argv[0]` as the command to be executed,
// and will perform a `PATH` lookup. If `command` were to be used instead,
// `CreateProcess` would require an absolute path.
//
// If `create_suspended` is `true`, the process will not be started, and the
// caller must use `ResumeThread` to start the process.
//
// The caller can specify explicit `stdin`, `stdout`, and `stderr` handles,
// in that order, for the process via the `pipes` argument.
//
// The return value is a `ProcessData` struct, with the process and thread
// handles each saved in a `SharedHandle`, ensuring they are closed when struct
// goes out of scope.
inline Try<ProcessData> create_process(
    const std::string& command,
    const std::vector<std::string>& argv,
    const Option<std::map<std::string, std::string>>& environment,
    const bool create_suspended = false,
    const Option<std::tuple<int_fd, int_fd, int_fd>> pipes = None())
{
  // TODO(andschwa): Assert that `command` and `argv[0]` are the same.
  std::wstring arg_string = stringify_args(argv);
  std::vector<wchar_t> arg_buffer(arg_string.begin(), arg_string.end());
  arg_buffer.push_back(L'\0');

  // Create the process with a Unicode environment.
  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
  if (create_suspended) {
    creation_flags |= CREATE_SUSPENDED;
  }

  // Construct the environment that will be passed to `::CreateProcessW`.
  Option<std::wstring> env_string = create_process_env(environment);
  std::vector<wchar_t> env_buffer;
  if (env_string.isSome()) {
    // This string contains the necessary null characters.
    env_buffer.assign(env_string.get().begin(), env_string.get().end());
  }

  wchar_t* process_env = env_buffer.empty() ? nullptr : env_buffer.data();

  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(PROCESS_INFORMATION));

  STARTUPINFOW startup_info;
  memset(&startup_info, 0, sizeof(STARTUPINFOW));
  startup_info.cb = sizeof(STARTUPINFOW);

  // Hook up the stdin/out/err pipes and use the `STARTF_USESTDHANDLES`
  // flag to instruct the child to use them [1].
  // A more user-friendly example can be found in [2].
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms686331(v=vs.85).aspx
  // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
  if (pipes.isSome()) {
    startup_info.hStdInput = std::get<0>(pipes.get());
    startup_info.hStdOutput = std::get<1>(pipes.get());
    startup_info.hStdError = std::get<2>(pipes.get());
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
  }

  BOOL create_process_result = ::CreateProcessW(
      // This is replaced by the first token of `arg_buffer` string.
      static_cast<LPCWSTR>(nullptr),
      static_cast<LPWSTR>(arg_buffer.data()),
      static_cast<LPSECURITY_ATTRIBUTES>(nullptr),
      static_cast<LPSECURITY_ATTRIBUTES>(nullptr),
      TRUE, // Inherited parent process handles.
      creation_flags,
      static_cast<LPVOID>(process_env),
      static_cast<LPCWSTR>(nullptr), // Inherited working directory.
      &startup_info,
      &process_info);

  if (!create_process_result) {
    return WindowsError(
        "Failed to call `CreateProcess`: " + stringify(arg_string));
  }

  return ProcessData{
    SharedHandle{process_info.hProcess, ::CloseHandle},
    SharedHandle{process_info.hThread, ::CloseHandle},
    static_cast<pid_t>(process_info.dwProcessId)};
}

} // namespace windows {

} // namespace internal {

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
Try<std::string> shell(const std::string& fmt, const T&... t) = delete;


template<typename... T>
inline int execlp(const char* file, T... t) = delete;


// Executes a command by calling "<command> <arguments...>", and
// returns after the command has been completed. Returns process exit code if
// succeeds, and -1 on error.
inline int spawn(
    const std::string& command,
    const std::vector<std::string>& arguments,
    const Option<std::map<std::string, std::string>>& environment = None())
{
  Try<::internal::windows::ProcessData> process_data =
    ::internal::windows::create_process(command, arguments, environment);

  if (process_data.isError()) {
    LOG(WARNING) << process_data.error();
    return -1;
  }

  // Wait for the process synchronously.
  ::WaitForSingleObject(
      process_data.get().process_handle.get_handle(), INFINITE);

  DWORD status;
  if (!::GetExitCodeProcess(
           process_data.get().process_handle.get_handle(),
           &status)) {
    LOG(WARNING) << "Failed to `GetExitCodeProcess`: " << command;
    return -1;
  }

  // Return the child exit code.
  return static_cast<int>(status);
}


// Executes a command by calling "cmd /c <command>", and returns
// after the command has been completed. Returns exit code if succeeds, and
// return -1 on error.
//
// Note: Be cautious about shell injection
// (https://en.wikipedia.org/wiki/Code_injection#Shell_injection)
// when using this method and use proper validation and sanitization
// on the `command`. For this reason in general `os::spawn` is
// preferred if a shell is not required.
inline int system(const std::string& command)
{
  return os::spawn(Shell::name, {Shell::arg0, Shell::arg1, command});
}


// In order to emulate the semantics of `execvp`, `os::spawn` waits for the new
// process to exit, and returns its error code, which is propogated back to the
// parent via `exit` here.
inline int execvp(
    const std::string& command,
    const std::vector<std::string>& argv)
{
  exit(os::spawn(command, argv));
  return -1;
}


// NOTE: This function can accept `Argv` and `Envp` constructs through their
// explicit type conversions, but unlike the POSIX implementations, it cannot
// accept the raw forms.
inline int execvpe(
    const std::string& command,
    const std::vector<std::string>& argv,
    const std::map<std::string, std::string>& envp)
{
  exit(os::spawn(command, argv, envp));
  return -1;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SHELL_HPP__
