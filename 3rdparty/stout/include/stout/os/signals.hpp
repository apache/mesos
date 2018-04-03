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

#ifndef __STOUT_OS_SIGNALS_HPP__
#define __STOUT_OS_SIGNALS_HPP__


// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
//
// NOTE: The `os::signals` namespace is not, and will not be,
// implemented on Windows. We do not throw an error error here so that
// the inclusion of this header does not need to guarded; however,
// uses of `os::signals` will need to be guarded.
#ifndef __WINDOWS__
#include <stout/os/posix/signals.hpp>

#define SUPPRESS(signal) \
  if (os::signals::internal::Suppressor suppressor ## signal = \
      os::signals::internal::Suppressor(signal))
#endif // __WINDOWS__


#endif // __STOUT_OS_SIGNALS_HPP__
