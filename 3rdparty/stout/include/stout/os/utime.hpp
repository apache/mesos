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

#ifndef __STOUT_OS_UTIME_HPP__
#define __STOUT_OS_UTIME_HPP__

#include <string>

#ifdef __WINDOWS__
#include <sys/utime.h>
#else
#include <utime.h>
#endif // __WINDOWS__

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace os {

// Sets the access and modification times of 'path' to the current time.
inline Try<Nothing> utime(const std::string& path)
{
  // NOTE: on Windows, this correctly dispatches to `_utime`, so there is no
  // need to create a function alias in `stout/windows.hpp` as we have done for
  // `stout/os/mkdir.hpp` and friends.
  if (::utime(path.c_str(), nullptr) == -1) {
    return ErrnoError();
  }

  return Nothing();
}

} // namespace os {


#endif // __STOUT_OS_UTIME_HPP__
