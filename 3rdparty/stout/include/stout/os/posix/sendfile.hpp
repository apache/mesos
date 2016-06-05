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

#ifndef __STOUT_OS_POSIX_SENDFILE_HPP__
#define __STOUT_OS_POSIX_SENDFILE_HPP__

#include <errno.h>

#if defined(__linux__) || defined(__sun)
#include <sys/sendfile.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#endif // __APPLE__ || __FreeBSD__

#include <stout/os/signals.hpp>
#include <stout/unreachable.hpp>

#include <stout/error.hpp>
#include <stout/try.hpp>

namespace os {

// Returns the amount of bytes written from the input file
// descriptor to the output socket. On error,
// `Try<ssize_t, SocketError>` contains the error.
// NOTE: The following limitations exist because of the OS X
// implementation of sendfile:
//   1. s must be a stream oriented socket descriptor.
//   2. fd must be a regular file descriptor.
inline Try<ssize_t, SocketError> sendfile(
    int s, int fd, off_t offset, size_t length)
{
#if defined(__linux__) || defined(__sun)
  SUPPRESS (SIGPIPE) {
    // This will set errno to EPIPE if a SIGPIPE occurs.
    ssize_t sent = ::sendfile(s, fd, &offset, length);
    if (sent < 0) {
      return SocketError();
    }

    return sent;
  }
  UNREACHABLE();
#elif defined __APPLE__
  // On OS X, sendfile does not need to have SIGPIPE suppressed.
  off_t _length = static_cast<off_t>(length);

  if (::sendfile(fd, s, offset, &_length, nullptr, 0) < 0) {
    if (errno == EAGAIN && _length > 0) {
      return _length;
    }
    return SocketError();
  }

  return _length;
#elif defined __FreeBSD__
  off_t _length = 0;

  SUPPRESS (SIGPIPE) {
      if (::sendfile(fd, s, offset, length, nullptr, &_length, 0) < 0) {
        if (errno == EAGAIN && length > 0) {
          return _length;
        }
        return SocketError();
      }
  }

  return _length;
#endif
}

} // namespace os {

#endif // __STOUT_OS_POSIX_SENDFILE_HPP__
