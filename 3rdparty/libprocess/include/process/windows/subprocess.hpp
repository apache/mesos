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

#ifndef __PROCESS_WINDOWS_SUBPROCESS_HPP__
#define __PROCESS_WINDOWS_SUBPROCESS_HPP__

#include <signal.h>

#include <string>

#include <glog/logging.h>

#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/close.hpp>
#include <stout/os/environment.hpp>

#include <userEnv.h>

namespace process {
namespace internal {

// Retrieves system environment in a `std::map`, ignoring
// the current process's environment variables.
inline Option<std::map<std::string, std::string>> getSystemEnvironment()
{
  std::wstring_convert<std::codecvt<wchar_t, char, mbstate_t>,
    wchar_t> converter;

  std::map<std::string, std::string> systemEnvironment;
  wchar_t* environmentEntry = nullptr;

  // Get the system environment.
  // The third parameter (bool) tells the function *not* to inherit
  // variables from the current process.
  if (!CreateEnvironmentBlock((LPVOID*)&environmentEntry, nullptr, FALSE)) {
    return None();
  }

  // Save the environment block in order to destroy it later.
  wchar_t* environmentBlock = environmentEntry;

  while (*environmentEntry != L'\0') {
    // Each environment block contains the environment variables as follows:
    // Var1=Value1\0
    // Var2=Value2\0
    // Var3=Value3\0
    // ...
    // VarN=ValueN\0\0
    // The name of an environment variable cannot include an equal sign (=).

    wchar_t * separator = wcschr(environmentEntry, L'=');
    std::wstring varName = std::wstring(environmentEntry, separator);
    std::wstring varVal = std::wstring(separator + 1);

    // Mesos variables are upper case. Convert system variables to
    // match the name provided by the scheduler in case of a collision.
    std::transform(varName.begin(), varName.end(), varName.begin(), ::towupper);

    // The system environment has priority. Force `ANSI` usage until the code
    // is converted to UNICODE.
    systemEnvironment.insert_or_assign(
      converter.to_bytes(varName.c_str()),
      converter.to_bytes(varVal.c_str()));

    environmentEntry += varName.length() + varVal.length() + 2;
  }

  DestroyEnvironmentBlock(environmentBlock);

  return systemEnvironment;
}

// Creates a null-terminated array of null-terminated strings that will be
// passed to `CreateProcess` as the `lpEnvironment` argument, as described by
// MSDN[1]. This array needs to be sorted in alphabetical order, but the `map`
// already takes care of that. Note that this function does not handle Unicode
// environments, so it should not be used in conjunction with the
// `CREATE_UNICODE_ENVIRONMENT` flag.
//
// NOTE: This function will add the system's environment variables into
// the returned string. These variables take precedence over the provided
// `env` and are generally necessary in order to launch things on Windows.
//
// [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682425(v=vs.85).aspx
inline Option<std::string> createProcessEnvironment(
    const Option<std::map<std::string, std::string>>& env)
{
  if (env.isNone() || (env.isSome() && env.get().size() == 0)) {
    return None();
  }

  Option<std::map<std::string, std::string>> systemEnvironment =
    getSystemEnvironment();

  // The system environment must be non-empty.
  // No subprocesses will be able to launch if the system environment is blank.
  CHECK(systemEnvironment.isSome() && systemEnvironment.get().size() > 0);

  std::map<std::string, std::string> combinedEnvironment = env.get();

  foreachpair (const std::string& key,
               const std::string& value,
               systemEnvironment.get()) {
    combinedEnvironment[key] = value;
  }

  std::string environmentString;
  foreachpair (const std::string& key,
               const std::string& value,
               combinedEnvironment) {
    environmentString += key + '=' + value + '\0';
  }

  return environmentString;
}


inline Try<PROCESS_INFORMATION> createChildProcess(
    const std::string& path,
    const std::vector<std::string>& argv,
    const Option<std::map<std::string, std::string>>& environment,
    const InputFileDescriptors stdinfds,
    const OutputFileDescriptors stdoutfds,
    const OutputFileDescriptors stderrfds,
    const std::vector<Subprocess::ParentHook>& parent_hooks)
{
  // Construct the environment that will be passed to `CreateProcess`.
  Option<std::string> environmentString = createProcessEnvironment(environment);
  const char* processEnvironment = environmentString.isNone()
    ? nullptr
    : environmentString.get().c_str();

  PROCESS_INFORMATION processInfo;
  STARTUPINFO startupInfo;

  ::ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));
  ::ZeroMemory(&startupInfo, sizeof(STARTUPINFO));

  // Hook up the `stdin`/`stdout`/`stderr` pipes and use the
  // `STARTF_USESTDHANDLES` flag to instruct the child to use them[1]. A more
  // user-friendly example can be found in [2].
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms686331(v=vs.85).aspx
  // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
  startupInfo.cb = sizeof(STARTUPINFO);
  startupInfo.hStdInput = stdinfds.read;
  startupInfo.hStdOutput = stdoutfds.write;
  startupInfo.hStdError = stderrfds.write;
  startupInfo.dwFlags |= STARTF_USESTDHANDLES;

  // Build command to pass to `::CreateProcess`.
  //
  // NOTE: We are expecting that components of `argv` that need to be quoted
  // (for example, paths with spaces in them like `C:\"Program Files"\foo.exe`)
  // to have been already quoted correctly before we generate `command`.
  // Incorrectly-quoted command arguments will probably lead the child process
  // to terminate with an error. See also NOTE on `process::subprocess`.
  std::string command = strings::join(" ", argv);

  // Escape the quotes in `command`.
  //
  // TODO(dpravat): Add tests cases that cover this functionality. See
  // MESOS-5418.
  command = strings::replace(command, "\"", "\\\"");

  // NOTE: If Mesos is built against the ANSI version of this function, the
  // environment is limited to 32,767 characters. See[1].
  //
  // TODO(hausdorff): Figure out how to map the `path` and `args` arguments of
  // this function into a call to `::CreateProcess` that is more general
  // purpose. In particular, on POSIX, we expect that calls to `subprocess` can
  // be called with relative `path` (e.g., it could simply be `sh`). However,
  // on Windows, unlike the calls to (e.g.) `exec`, `::CreateProcess` will
  // expect that this argument be a fully-qualified path. In the end, we'd like
  // the calls to `subprocess` to have similar command formats to minimize
  // confusion and mistakes.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682425(v=vs.85).aspx
  BOOL createProcessResult = CreateProcess(
      nullptr,
      (LPSTR)command.data(),
      nullptr,                 // Default security attributes.
      nullptr,                 // Default primary thread security attributes.
      TRUE,                    // Inherited parent process handles.
      CREATE_SUSPENDED,        // Create process in suspended state.
      (LPVOID)processEnvironment,
      nullptr,                 // Use parent's current directory.
      &startupInfo,            // STARTUPINFO pointer.
      &processInfo);           // PROCESS_INFORMATION pointer.

  if (!createProcessResult) {
    return WindowsError(
        "Failed to call CreateProcess on command '" + command + "'");
  }

  // Run the parent hooks.
  const pid_t pid = processInfo.dwProcessId;
  foreach (const Subprocess::ParentHook& hook, parent_hooks) {
    Try<Nothing> parentSetup = hook.parent_setup(pid);

    // If the hook callback fails, we shouldn't proceed with the
    // execution and hence the child process should be killed.
    if (parentSetup.isError()) {
      // Attempt to kill the process. Since it is still in suspended state, we
      // do not need to kill any descendents. We also can't use `os::kill_job`
      // because this process is not in a Job Object unless one of the parent
      // hooks added it.
      ::TerminateProcess(processInfo.hProcess, 1);

      return Error(
          "Failed to execute Parent Hook in child '" + stringify(pid) +
          "' with command '" + command + "': " + parentSetup.error());
    }
  }

  // Start child process.
  if (::ResumeThread(processInfo.hThread) == -1) {
    return WindowsError(
        "Failed to resume child process with command '" + command + "'");
  }

  return processInfo;
}

}  // namespace internal {
}  // namespace process {

#endif // __PROCESS_WINDOWS_SUBPROCESS_HPP__
