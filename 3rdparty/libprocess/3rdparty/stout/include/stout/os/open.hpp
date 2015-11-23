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

#ifndef __STOUT_OS_OPEN_HPP__
#define __STOUT_OS_OPEN_HPP__

#include <sys/stat.h>
#include <sys/types.h>

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>


// For old systems that do not support O_CLOEXEC, we still want
// os::open to accept that flag so that we can simplify the code.
#ifndef O_CLOEXEC
// Since we will define O_CLOEXEC if it is not yet defined, we use a
// special symbol to tell if the flag is truly unavailable or not.
#define O_CLOEXEC_UNDEFINED

// NOTE: For backward compatibility concern, kernel usually does not
// change the constant values for symbols like O_CLOEXEC.
#if defined(__APPLE__)
// Copied from '/usr/include/sys/fcntl.h'
#define O_CLOEXEC 0x1000000
#elif defined(__linux__)
// Copied from '/usr/include/asm-generic/fcntl.h'.
#define O_CLOEXEC 02000000
#elif defined(__sun)
// Not defined on Solaris, taking a spare flag.
#define O_CLOEXEC 0x1000000
#endif // __ APPLE__
#endif // O_CLOEXEC

// Only include `fcntl` when strictly necessary, i.e., when we need to use
// `os::cloexec` to set the close-on-exec behavior of a file descriptor. We do
// this because some platforms (like Windows) will probably never support
// `os::cloexec`, and hence referencing that header will cause problems on some
// systems.
#ifdef O_CLOEXEC_UNDEFINED
#include <stout/os/fcntl.hpp>
#endif // O_CLOEXEC_UNDEFINED


namespace os {

inline Try<int> open(const std::string& path, int oflag, mode_t mode = 0)
{
#ifdef O_CLOEXEC_UNDEFINED
  // Before we passing oflag to ::open, we need to strip the O_CLOEXEC
  // flag since it's not supported.
  bool cloexec = false;
  if ((oflag & O_CLOEXEC) != 0) {
    oflag &= ~O_CLOEXEC;
    cloexec = true;
  }
#endif

  int fd = ::open(path.c_str(), oflag, mode);

  if (fd < 0) {
    return ErrnoError();
  }

#ifdef O_CLOEXEC_UNDEFINED
  if (cloexec) {
    Try<Nothing> result = os::cloexec(fd);
    if (result.isError()) {
      os::close(fd);
      return Error("Failed to set cloexec: " + result.error());
    }
  }
#endif

  return fd;
}

} // namespace os {

#endif // __STOUT_OS_OPEN_HPP__
