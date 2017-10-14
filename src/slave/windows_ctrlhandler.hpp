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

#ifndef __WINDOWS_CTRLHANDLER_HPP__
#define __WINDOWS_CTRLHANDLER_HPP__

#include <functional>
#include <mutex>

#include <stout/synchronized.hpp>

namespace os {
namespace internal {

#define SIGUSR1 100

// Not using extern as this is used only by the executable. The signal
// handler should be configured once. Configuring it multiple times will
// overwrite any previous handlers.
static std::function<void(int, int)>* signaledWrapper = nullptr;

inline BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
  switch (fdwCtrlType) {
  // Handle the CTRL-C signal.
  case CTRL_C_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT: {
    if (signaledWrapper != nullptr) {
      (*signaledWrapper)(SIGUSR1, 0);
      return TRUE;
    }
  }
  default: {
    return FALSE;
    }
  }
}


inline int installCtrlHandler(const std::function<void(int, int)>* signal)
{
  // NOTE: We only expect this function to be called multiple
  // times inside tests and `mesos-local`.

  static std::mutex mutex;

  synchronized (mutex) {
    if (signaledWrapper != nullptr) {
      delete signaledWrapper;
    }

    if (signal != nullptr) {
      signaledWrapper = new std::function<void(int, int)>(*signal);
      return SetConsoleCtrlHandler(CtrlHandler, TRUE);
    } else {
      delete signaledWrapper;
      signaledWrapper = nullptr;
      return SetConsoleCtrlHandler(CtrlHandler, FALSE);
    }
  }
}

} // namespace internal {

} // namespace os {

#endif // __WINDOWS_CTRLHANDLER_HPP__
