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

#ifndef __STOUT_OS_WINDOWS_BOOTID_HPP__
#define __STOUT_OS_WINDOWS_BOOTID_HPP__

#include <chrono>
#include <string>

#include <stout/stringify.hpp>
#include <stout/try.hpp>


namespace os {

inline Try<std::string> bootId()
{
  // NOTE: we follow the precedent of the OS X design here and use the boot
  // time in seconds since the Unix epoch as a boot ID. See comment in
  // `stout/os/posix/bootid.hpp` for discussion of this approach. Note also
  // that we can't use milliseconds here instead of seconds because the
  // relatively high imprecision of millisecond resolution would cause `bootId`
  // to return a different number nearly every time it was called.

  std::chrono::milliseconds uptime =
    std::chrono::milliseconds(GetTickCount64());

  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  std::chrono::system_clock::time_point boot_time = now - uptime;

  long long boot_time_secs = std::chrono::duration_cast<std::chrono::seconds>(
      boot_time.time_since_epoch()).count();

  return stringify(boot_time_secs);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_BOOTID_HPP__
