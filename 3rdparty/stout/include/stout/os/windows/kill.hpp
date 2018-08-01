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

#ifndef __STOUT_OS_WINDOWS_KILL_HPP__
#define __STOUT_OS_WINDOWS_KILL_HPP__

#include <glog/logging.h>

#include <stout/os.hpp>
#include <stout/windows.hpp>

namespace os {

const int KILL_PASS = 0; // Success return for `kill` function.
const int KILL_FAIL = -1; // Error return for `kill` function.

namespace internal {

inline int kill_process(pid_t pid)
{
  HANDLE process_handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (process_handle == nullptr) {
    LOG(ERROR) << "os::kill_process(): Failed call to OpenProcess";

    return KILL_FAIL;
  }

  SharedHandle safe_process_handle(process_handle, ::CloseHandle);

  if (::TerminateProcess(safe_process_handle.get_handle(), 1) == 0) {
    LOG(ERROR) << "os::kill_process(): Failed call to TerminateProcess";

    return KILL_FAIL;
  }

  return KILL_PASS;
}

} // namespace internal {


inline int kill(pid_t pid, int sig)
{
  // SIGCONT and SIGSTOP are not supported.
  // SIGKILL calls TerminateProcess.
  // SIGTERM has the same behaviour as SIGKILL.

  if (sig == SIGKILL || sig == SIGTERM) {
    return os::internal::kill_process(pid);
  }

  LOG(ERROR) << "Failed call to os::kill(): "
             << "Signal value: '" << sig << "' is not handled. "
             << "Valid Signal values for Windows os::kill() are "
             << "'SIGTERM' and 'SIGKILL'";

  return KILL_FAIL;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_KILL_HPP__
