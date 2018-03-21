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

#ifndef __STOUT_OS_WINDOWS_SOCKET_HPP__
#define __STOUT_OS_WINDOWS_SOCKET_HPP__

#include <glog/logging.h>

#include <stout/abort.hpp>
#include <stout/try.hpp>
#include <stout/error.hpp>
#include <stout/windows.hpp> // For `WinSock2.h`.

#include <stout/os/int_fd.hpp>

namespace net {

// Initialize Windows socket stack.
inline bool wsa_initialize()
{
  // Initialize WinSock (request version 2.2).
  WORD requestedVersion = MAKEWORD(2, 2);
  WSADATA data;

  const int result = ::WSAStartup(requestedVersion, &data);
  if (result != 0) {
    const int error = ::WSAGetLastError();
    LOG(ERROR) << "Could not initialize WinSock, error code : " << error;
    return false;
  }

  // Check that the WinSock version we got back is 2.2 or higher.
  // The high-order byte specifies the minor version number.
  if (LOBYTE(data.wVersion) < 2 ||
      (LOBYTE(data.wVersion) == 2 && HIBYTE(data.wVersion) != 2)) {
    LOG(ERROR) << "Incorrect WinSock version found : " << LOBYTE(data.wVersion)
               << "." << HIBYTE(data.wVersion);

    // WinSock was initialized, we just didn't like the version, so we need to
    // clean up.
    if (::WSACleanup() != 0) {
      const int error = ::WSAGetLastError();
      LOG(ERROR) << "Could not cleanup WinSock, error code : " << error;
    }

    return false;
  }

  return true;
}


inline bool wsa_cleanup()
{
  // Cleanup WinSock. Wait for any outstanding socket operations to complete
  // before exiting. Retry for a maximum of 10 times at 1 second intervals.
  int retriesLeft = 10;

  while (retriesLeft > 0) {
    const int result = ::WSACleanup();
    if (result != 0) {
      const int error = ::WSAGetLastError();
      // Make it idempotent.
      if (error == WSANOTINITIALISED) {
        return false;
      }

      // Wait for any blocking calls to complete and retry after 1 second.
      if (error == WSAEINPROGRESS) {
        LOG(ERROR) << "Waiting for outstanding WinSock calls to complete.";
        ::Sleep(1000);
        retriesLeft--;
      } else {
        LOG(ERROR) << "Could not cleanup WinSock, error code : " << error;
        return false;
      }
    }
    break;
  }
  if (retriesLeft == 0) {
    return false;
  }

  return true;
}


// The error indicates the last socket operation has been
// interupted, the operation can be restarted immediately.
// The error will append on Windows only when the operation
// is interupted using  `WSACancelBlockingCall`.
inline bool is_restartable_error(int error) { return (error == WSAEINTR); }


// The error indicates the last socket function on a non-blocking socket
// cannot be completed. This is a temporary condition and the caller can
// retry the operation later.
inline bool is_retryable_error(int error) { return (error == WSAEWOULDBLOCK); }
inline bool is_inprogress_error(int error) { return (error == WSAEWOULDBLOCK); }


// Returns a socket file descriptor for the specified options.
//
// NOTE: We default to no inheritance because we never inherit sockets.
// Overlapped I/O is enabled to match the default behavior of `::socket`.
inline Try<int_fd> socket(
    int family,
    int type,
    int protocol,
    DWORD flags = WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)
{
  SOCKET s = ::WSASocketW(family, type, protocol, nullptr, 0, flags);
  if (s == INVALID_SOCKET) {
    return WindowsSocketError();
  }

  return s;
}

// NOTE: The below wrappers are used to silence some implicit
// type-casting warnings.

inline int_fd accept(
    const int_fd& fd, sockaddr* addr, socklen_t* addrlen)
{
  return int_fd(::accept(fd, addr, reinterpret_cast<int*>(addrlen)));
}


// NOTE: If `::bind` or `::connect` fail, they return `SOCKET_ERROR`, which is
// defined to be `-1`. Therefore, the error checking logic of `result < 0` used
// on POSIX will also work on Windows.

inline int bind(
    const int_fd& fd, const sockaddr* addr, socklen_t addrlen)
{
  CHECK_LE(addrlen, INT32_MAX);
  return ::bind(fd, addr, static_cast<int>(addrlen));
}


inline int connect(
    const int_fd& fd, const sockaddr* address, socklen_t addrlen)
{
  CHECK_LE(addrlen, INT32_MAX);
  return ::connect(fd, address, static_cast<int>(addrlen));
}


inline ssize_t send(
    const int_fd& fd, const void* buf, size_t len, int flags)
{
  CHECK_LE(len, INT32_MAX);
  return ::send(
      fd, static_cast<const char*>(buf), static_cast<int>(len), flags);
}


inline ssize_t recv(const int_fd& fd, void* buf, size_t len, int flags)
{
  CHECK_LE(len, INT32_MAX);
  return ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), flags);
}

} // namespace net {

#endif // __STOUT_OS_WINDOWS_SOCKET_HPP__
