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

#ifndef __STOUT_OS_POSIX_BOOTID_HPP__
#define __STOUT_OS_POSIX_BOOTID_HPP__

#include <string>

#include <sys/time.h>

#include <stout/error.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/read.hpp>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stout/os/sysctl.hpp>
#endif // __APPLE__ || __FreeBSD__


namespace os {

inline Try<std::string> bootId()
{
#ifdef __linux__
  Try<std::string> read = os::read("/proc/sys/kernel/random/boot_id");
  if (read.isError()) {
    return read;
  }
  return strings::trim(read.get());
#elif defined(__APPLE__) || defined(__FreeBSD__)
  // For OS X, we use the boot time in seconds as a unique boot id.
  // Although imperfect, this works quite well in practice. NOTE: we can't use
  // milliseconds here instead of seconds because the relatively high
  // imprecision of millisecond resolution would cause `bootId` to return a
  // different number nearly every time it was called.
  Try<timeval> boot_time = os::sysctl(CTL_KERN, KERN_BOOTTIME).time();
  if (boot_time.isError()) {
    return Error(boot_time.error());
  }
  return stringify(boot_time->tv_sec);
#else
  return Error("Not implemented");
#endif
}

} // namespace os {

#endif // __STOUT_OS_POSIX_BOOTID_HPP__
