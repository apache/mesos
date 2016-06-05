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
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>
#include <stout/os/environment.hpp>

using std::map;
using std::string;
using std::vector;


namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;

namespace internal {

// This function will invoke `os::close` on all specified file
// descriptors that are valid (i.e., not `None` and >= 0).
inline void close(
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds)
{
  HANDLE fds[6] = {
    stdinfds.read, stdinfds.write.getOrElse(INVALID_HANDLE_VALUE),
    stdoutfds.read.getOrElse(INVALID_HANDLE_VALUE), stdoutfds.write,
    stderrfds.read.getOrElse(INVALID_HANDLE_VALUE), stderrfds.write
  };

  foreach (HANDLE fd, fds) {
    if (fd != INVALID_HANDLE_VALUE) {
      os::close(fd);
    }
  }
}


// Creates a null-terminated array of null-terminated strings that will be
// passed to `CreateProcess` as the `lpEnvironment` argument, as described by
// MSDN[1]. This array needs to be sorted in alphabetical order, but the `map`
// already takes care of that. Note that this function does not handle Unicode
// environments, so it should not be used in conjunction with the
// `CREATE_UNICODE_ENVIRONMENT` flag.
//
// [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682425(v=vs.85).aspx
inline Option<string> createProcessEnvironment(
    const Option<map<string, string>>& env)
{
  if (env.isNone() || (env.isSome() && env.get().size() == 0)) {
    return None();
  }

  string environmentString;
  foreachpair (const string& key, const string& value, env.get()) {
    environmentString += key + '=' + value + '\0';
  }

  return environmentString;
}


inline Try<PROCESS_INFORMATION> createChildProcess(
    const string& path,
    const vector<string>& argv,
    const Option<map<string, string>>& environment,
    const InputFileDescriptors stdinfds,
    const OutputFileDescriptors stdoutfds,
    const OutputFileDescriptors stderrfds)
{
  // Construct the environment that will be passed to `CreateProcess`.
  Option<string> environmentString = createProcessEnvironment(environment);
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
  string command = strings::join(" ", argv);

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
      0,                       // Normal thread priority.
      (LPVOID)processEnvironment,
      nullptr,                 // Use parent's current directory.
      &startupInfo,            // STARTUPINFO pointer.
      &processInfo);           // PROCESS_INFORMATION pointer.

  if (!createProcessResult) {
    return WindowsError("createChildProcess: failed to call 'CreateProcess'");
  }

  return processInfo;
}

}  // namespace internal {
}  // namespace process {

#endif // __PROCESS_WINDOWS_SUBPROCESS_HPP__
