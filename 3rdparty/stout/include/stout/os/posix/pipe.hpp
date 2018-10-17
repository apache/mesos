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

#ifndef __STOUT_OS_POSIX_PIPE_HPP__
#define __STOUT_OS_POSIX_PIPE_HPP__

#include <unistd.h>

#include <sys/syscall.h>

#include <array>

#include <stout/error.hpp>
#include <stout/try.hpp>

#include <stout/os/posix/fcntl.hpp>

namespace os {

// Create pipes for interprocess communication. The pipe file descriptors
// will be marked O_CLOEXEC (atomically if the platform supports it). To
// pass the pipe to a child process, the caller should clear the CLOEXEC
// flag after fork(2) but before exec(2).
inline Try<std::array<int, 2>> pipe()
{
  std::array<int, 2> result;

  // The pipe2() function appeared in FreeBSD 10.0.
#if defined(__FreeBSD__) && __FreeBSD_version >= 1000000

  if (::pipe2(result.data(), O_CLOEXEC) < 0) {
    return ErrnoError();
  }

#else

  // pipe2() appeared in Linux 2.6.27 and glibc 2.9.
#if defined(__linux__) && defined(SYS_pipe2)
  if (::syscall(SYS_pipe2, result.data(), O_CLOEXEC) == 0) {
    return result;
  }

  // Fall back if the kernel doesn't support pipe2().
  if (errno != ENOSYS) {
    return ErrnoError();
  }
#endif

  if (::pipe(result.data()) < 0) {
    return ErrnoError();
  }

  Try<Nothing> cloexec = Nothing();

  cloexec = os::cloexec(result[0]);
  if (cloexec.isError()) {
    Error error = Error("Failed to cloexec pipe: " + cloexec.error());
    ::close(result[0]);
    ::close(result[1]);
    return error;
  }

  cloexec = os::cloexec(result[1]);
  if (cloexec.isError()) {
    Error error = Error("Failed to cloexec pipe: " + cloexec.error());
    ::close(result[0]);
    ::close(result[1]);
    return error;
  }

#endif

  return result;
}

} // namespace os {

#endif // __STOUT_OS_POSIX_PIPE_HPP__
