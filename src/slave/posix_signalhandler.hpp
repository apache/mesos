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

#ifndef __POSIX_SIGNALHANDLER_HPP__
#define __POSIX_SIGNALHANDLER_HPP__

#include <signal.h>

#include <functional>
#include <mutex>

#include <stout/synchronized.hpp>

namespace os {
namespace internal {

// Not using extern as this is used only by the executable. The signal
// handler should be configured once. Configuring it multiple times will
// overwrite any previous handlers.
std::function<void(int, int)>* signaledWrapper = nullptr;

void signalHandler(int sig, siginfo_t* siginfo, void* context)
{
  if (signaledWrapper != nullptr) {
     (*signaledWrapper)(sig, siginfo->si_uid);
  }
}


int configureSignal(const std::function<void(int, int)>* signal)
{
  // NOTE: We only expect this function to be called multiple
  // times inside tests and `mesos-local`.

  static std::mutex mutex;

  synchronized (mutex) {
    if (signaledWrapper != nullptr) {
      delete signaledWrapper;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));

    signaledWrapper = new std::function<void(int, int)>(*signal);

    // Do not block additional signals while in the handler.
    sigemptyset(&action.sa_mask);

    // The SA_SIGINFO flag tells `sigaction()` to use
    // the sa_sigaction field, not sa_handler.
    action.sa_flags = SA_SIGINFO;

    action.sa_sigaction = signalHandler;

    return sigaction(SIGUSR1, &action, nullptr);
  }
}

} // namespace internal {

} // namespace os {

#endif // __POSIX_SIGNALHANDLER_HPP__
