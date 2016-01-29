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

#ifndef __STOUT_OS_WINDOWS_SIGNALS_HPP__
#define __STOUT_OS_WINDOWS_SIGNALS_HPP__

#include <errno.h>
#include <signal.h>
#include <string.h>


namespace os {

namespace signals {

// Installs the given signal handler.
inline int install(int signal, void(*handler)(int))
{
  UNIMPLEMENTED;
}


// Resets the signal handler to the default handler of the signal.
inline int reset(int signal)
{
  UNIMPLEMENTED;
}


// Returns true iff the signal is pending.
inline bool pending(int signal)
{
  UNIMPLEMENTED;
}


// Returns true if the signal has been blocked, or false if the
// signal was already blocked.
inline bool block(int signal)
{
  UNIMPLEMENTED;
}


// Returns true if the signal has been unblocked, or false if the
// signal was not previously blocked.
inline bool unblock(int signal)
{
  UNIMPLEMENTED;
}

namespace internal {

// Suppresses a signal on the current thread for the lifetime of
// the Suppressor. The signal *must* be synchronous and delivered
// per-thread. The suppression occurs only on the thread of
// execution of the Suppressor.
struct Suppressor
{
  Suppressor(int _signal)
    : signal(_signal), pending(false), unblock(false)
  {
    UNIMPLEMENTED;
  }

  ~Suppressor()
  {
    UNIMPLEMENTED;
  }

  // Needed for the SUPPRESS() macro.
  operator bool() { return true; }
};

} // namespace internal {

} // namespace signals {

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SIGNALS_HPP__
