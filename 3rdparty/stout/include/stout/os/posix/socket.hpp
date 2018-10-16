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

#ifndef __STOUT_OS_POSIX_SOCKET_HPP__
#define __STOUT_OS_POSIX_SOCKET_HPP__

#include <array>

#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/fcntl.hpp>

namespace net {

// Import `socket` functions into `net::` namespace.
using ::accept;
using ::bind;
using ::connect;
using ::recv;
using ::send;


// Returns a socket file descriptor for the specified options.
// NOTE: on OS X, the returned socket will have the SO_NOSIGPIPE option set.
inline Try<int_fd> socket(int family, int type, int protocol)
{
  int_fd s;
  if ((s = ::socket(family, type, protocol)) < 0) {
    return ErrnoError();
  }

#ifdef __APPLE__
  // Disable SIGPIPE via setsockopt because OS X does not support
  // the MSG_NOSIGNAL flag on send(2).
  const int enable = 1;
  if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(int)) == -1) {
    return ErrnoError();
  }
#endif // __APPLE__

  return s;
}


// The error indicates the last socket operation has been
// interupted, the operation can be restarted immediately.
inline bool is_restartable_error(int error)
{
  return (error == EINTR);
}


// The error indicates the last socket function on a non-blocking socket
// cannot be completed. This is a temporary condition and the caller can
// retry the operation later.
inline bool is_retryable_error(int error)
{
  return (error == EWOULDBLOCK || error == EAGAIN);
}


inline bool is_inprogress_error(int error)
{
  return (error == EINPROGRESS);
}


inline bool is_socket(int fd)
{
  struct stat statbuf;
  if (::fstat(fd, &statbuf) < 0) {
    return false;
  }

  return S_ISSOCK(statbuf.st_mode) != 0;
}


inline Try<std::array<int_fd, 2>> socketpair(int family, int type, int protocol)
{
  std::array<int_fd, 2> result;

#if __APPLE__ || !defined(SOCK_CLOEXEC)
  auto close = [](const std::array<int_fd, 2>& fds) {
    int errsav = errno;
    ::close(fds[0]);
    ::close(fds[1]);
    errno = errsav;
  };
#endif

#if defined(SOCK_CLOEXEC)
  type |= SOCK_CLOEXEC;
#endif

  if (::socketpair(family, type, 0, result.data()) != 0) {
    return ErrnoError();
  }

#if !defined(SOCK_CLOEXEC)
  Try<Nothing> cloexec = Nothing();

  cloexec = os::cloexec(result[0]);
  if (cloexec.isError()) {
    close(result);
    return Error("Failed to cloexec socket: " + cloexec.error());
  }

  cloexec = os::cloexec(result[1]);
  if (cloexec.isError()) {
    close(result);
    return Error("Failed to cloexec socket: " + cloexec.error());
  }
#endif

#ifdef __APPLE__
  // Disable SIGPIPE to be consistent with net::socket().
  const int enable = 1;

  if (::setsockopt(
        result[0],
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &enable,
        sizeof(enable)) == -1) {
    close(result);
    return ErrnoError("Failed to clear sigpipe");
  }

  if (::setsockopt(
        result[1],
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &enable,
        sizeof(enable)) == -1) {
    close(result);
    return ErrnoError("Failed to clear sigpipe");
  }
#endif // __APPLE__

  return result;
}

} // namespace net {

#endif // __STOUT_OS_POSIX_SOCKET_HPP__
