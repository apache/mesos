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

#ifndef __STOUT_OS_WINDOWS_EXEC_HPP__
#define __STOUT_OS_WINDOWS_EXEC_HPP__

#include <process.h>
#include <processthreadsapi.h>
#include <synchapi.h>
#include <userenv.h>

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

#include <stout/internal/windows/inherit.hpp>

namespace os {
namespace windows {
namespace internal {

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

  // Populate the combined environment first with the system environment.
  foreachpair (const std::wstring& key,
               const std::wstring& value,
               system_env.get()) {
    combined_env[key] = value;
  }

  // Now override with the supplied environment.
  foreachpair (const std::string& key,
               const std::string& value,
               env.get()) {
    combined_env[wide_stringify(key)] = wide_stringify(value);
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


struct ProcessData
{
  SharedHandle process_handle;
  SharedHandle thread_handle;
  pid_t pid;
};


// Provides an interface for creating a child process on Windows.
//
// The `command` argument is given for compatibility, and is ignored. This is
// because the `CreateProcess` will use the first part of `commandLine` as the
// module name to execute, and will perform a `PATH` lookup. If `command` were
// to be used instead, `CreateProcess` would require an absolute path.
// See the MSDN documentation for the complicated rules at play for parsing
// and locating the module to execute.
//
// This takes a string command line because Windows programs receive the
// entire command line as a string and perform their own parsing (unlike
// POSIX exec which allows you to explicitly pass arguments). However,
// many programs (e.g. those written with a libc `main(int argc, char** argv)`)
// will use `CommandLineToArgvW` to parse the command line. For callers
// of this function that want to pass arguments to such programs, we
// provide an overload of `create_process` that takes arguments
// explicitly and constructs an appropriate command line string for
// `CommandLineToArgvW` to parse it back out as arguments. Notably,
// cmd.exe has its own handling of the command line that differs from
// `CommandLineToArgvW`.
//
// If `create_suspended` is `true`, the process will not be started, and the
// caller must use `ResumeThread` to start the process.
//
// The caller can specify explicit `stdin`, `stdout`, and `stderr` handles,
// in that order, for the process via the `pipes` argument.
//
// NOTE: If `pipes` are specified, they will be temporarily set to
// inheritable, and then set to uninheritable. This is a side effect
// on each `HANDLE`.
//
// The return value is a `ProcessData` struct, with the process and thread
// handles each saved in a `SharedHandle`, ensuring they are closed when struct
// goes out of scope.
inline Try<ProcessData> create_process(
    const std::string& command,
    const std::vector<std::string>& argv,
    const Option<std::map<std::string, std::string>>& environment,
    const bool create_suspended = false,
    const Option<std::array<int_fd, 3>>& pipes = None(),
    const std::vector<int_fd>& whitelist_fds = {})
{
  // TODO(andschwa): Assert that `command` and `argv[0]` are the same.
  const std::wstring arg_string = stringify_args(argv);
  std::vector<wchar_t> arg_buffer(arg_string.begin(), arg_string.end());
  arg_buffer.push_back(L'\0');

  // Create the process with a Unicode environment and extended
  // startup info.
  DWORD creation_flags =
    CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
  if (create_suspended) {
    creation_flags |= CREATE_SUSPENDED;
  }

  // Construct the environment that will be passed to `::CreateProcessW`.
  const Option<std::wstring> env_string = create_process_env(environment);
  std::vector<wchar_t> env_buffer;
  if (env_string.isSome()) {
    // This string contains the necessary null characters.
    env_buffer.assign(env_string.get().begin(), env_string.get().end());
  }

  wchar_t* process_env = env_buffer.empty() ? nullptr : env_buffer.data();

  PROCESS_INFORMATION process_info = {};

  STARTUPINFOEXW startup_info_ex = {};
  startup_info_ex.StartupInfo.cb = sizeof(startup_info_ex);

  // Windows provides a way to whitelist a set of handles to be
  // inherited by the child process.
  // https://blogs.msdn.microsoft.com/oldnewthing/20111216-00/?p=8873
  // (1) We're setting the pipe handles and whitelisted handles to be
  //     temporarily inheritable.
  // (2) We're explicitly whitelisting the handles using a Windows API.
  // (3) We're then setting the handles to back to non-inheritable
  //     after the child process has been created.
  std::vector<HANDLE> handles;
  if (pipes.isSome()) {
    // Each of these handles must be inheritable.
    foreach (const int_fd& fd, pipes.get()) {
      handles.emplace_back(static_cast<HANDLE>(fd));
      const Try<Nothing> inherit = ::internal::windows::set_inherit(fd, true);
      if (inherit.isError()) {
        return Error(inherit.error());
      }
    }

    // Hook up the stdin/out/err pipes and use the `STARTF_USESTDHANDLES`
    // flag to instruct the child to use them [1].
    // A more user-friendly example can be found in [2].
    //
    // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms686331(v=vs.85).aspx
    // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
    startup_info_ex.StartupInfo.dwFlags   |= STARTF_USESTDHANDLES;
    startup_info_ex.StartupInfo.hStdInput  = std::get<0>(pipes.get());
    startup_info_ex.StartupInfo.hStdOutput = std::get<1>(pipes.get());
    startup_info_ex.StartupInfo.hStdError  = std::get<2>(pipes.get());
  }

  foreach (const int_fd& fd, whitelist_fds) {
    handles.emplace_back(static_cast<HANDLE>(fd));
    const Try<Nothing> inherit = ::internal::windows::set_inherit(fd, true);
    if (inherit.isError()) {
      return Error(inherit.error());
    }
  }

  Result<std::shared_ptr<::internal::windows::AttributeList>> attribute_list =
      ::internal::windows::create_attributes_list_for_handles(handles);

  if (attribute_list.isError()) {
    return Error(attribute_list.error());
  }

  if (attribute_list.isSome()) {
    startup_info_ex.lpAttributeList = attribute_list->get();
  }

  const BOOL result = ::CreateProcessW(
      // This is replaced by the first token of `wideCommandLineCopy` string.
      static_cast<LPCWSTR>(nullptr),
      static_cast<LPWSTR>(arg_buffer.data()),
      static_cast<LPSECURITY_ATTRIBUTES>(nullptr),
      static_cast<LPSECURITY_ATTRIBUTES>(nullptr),
      TRUE, // Inherit parent process handles (such as those in `pipes`).
      creation_flags,
      static_cast<LPVOID>(process_env),
      static_cast<LPCWSTR>(nullptr), // Inherit working directory.
      &startup_info_ex.StartupInfo,
      &process_info);

  // Save the error from the previous call so that we can proceed to
  // always revert the inheritance of the handles, and then report
  // this error, if there was one.
  const DWORD create_process_error = ::GetLastError();

  // NOTE: The MSDN documentation for `CreateProcess` states that it
  // returns before the process has "finished initialization," but is
  // not clear on precisely what initialization entails. It would seem
  // that this does not affect inherited handles, as it stands to
  // reason that the system call to `CreateProcess` causes inheritable
  // handles to become inherited, and not some "initialization" of the
  // child process. However, if an inheritance race condition
  // manifests, this assumption should be re-evaluated.
  if (pipes.isSome()) {
    // These handles should no longer be inheritable. This prevents other child
    // processes from accidentally inheriting the wrong handles.
    //
    // NOTE: This is explicit, and does not take into account the
    // previous inheritance semantics of each `HANDLE`. It is assumed
    // that users of this function send non-inheritable handles.
    foreach (const int_fd& fd, pipes.get()) {
      const Try<Nothing> inherit = ::internal::windows::set_inherit(fd, false);
      if (inherit.isError()) {
        return Error(inherit.error());
      }
    }
  }

  foreach (const int_fd& fd, whitelist_fds) {
    const Try<Nothing> inherit = ::internal::windows::set_inherit(fd, false);
    if (inherit.isError()) {
      return Error(inherit.error());
    }
  }

  if (result == FALSE) {
    return WindowsError(
        create_process_error,
        "Failed to call `CreateProcess`: " + stringify(arg_string));
  }

  return ProcessData{SharedHandle{process_info.hProcess, ::CloseHandle},
                     SharedHandle{process_info.hThread, ::CloseHandle},
                     static_cast<pid_t>(process_info.dwProcessId)};
}

} // namespace internal {
} // namespace windows {


inline Option<int> spawn(
    const std::string& command,
    const std::vector<std::string>& arguments,
    const Option<std::map<std::string, std::string>>& environment = None())
{
  using namespace os::windows::internal;

  Try<ProcessData> process_data =
    create_process(command, arguments, environment);

  if (process_data.isError()) {
    LOG(WARNING) << process_data.error();
    return None();
  }

  // Wait for the process synchronously.
  ::WaitForSingleObject(process_data->process_handle.get_handle(), INFINITE);

  DWORD status;
  if (!::GetExitCodeProcess(
        process_data->process_handle.get_handle(), &status)) {
    LOG(WARNING) << "Failed to `GetExitCodeProcess`: " << command;
    return None();
  }

  // Return the child exit code.
  return static_cast<int>(status);
}


inline int execvp(
    const std::string& file,
    const std::vector<std::string>& argv)
{
  exit(os::spawn(file, argv).getOrElse(-1));
  return -1;
}


inline int execvpe(
    const std::string& file,
    const std::vector<std::string>& argv,
    const std::map<std::string, std::string>& envp)
{
  exit(os::spawn(file, argv, envp).getOrElse(-1));
  return -1;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_EXEC_HPP__
