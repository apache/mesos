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

#include <winsock.h>

#include <glog/logging.h>

#include <stout/abort.hpp>

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
      (LOBYTE(data.wVersion) == 2 &&
      HIBYTE(data.wVersion) != 2)) {
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
// interupted, the operation can be restarted imediately.
// The error will append on Windows only when the operation
// is interupted using  `WSACancelBlockingCall`.
inline bool is_restartable_error(int error)
{
  return (error == WSAEINTR);
}


// The error indicates the last socket function on a non-blocking socket
// cannot be completed. This is a temporary condition and the caller can
// retry the operation later.
inline bool is_retryable_error(int error)
{
  return (error == WSAEWOULDBLOCK);
}


inline bool is_inprogress_error(int error)
{
  return (error == WSAEWOULDBLOCK);
}


inline bool is_socket(SOCKET fd)
{
  int value = 0;
  int length = sizeof(int);

  if (::getsockopt(
          fd,
          SOL_SOCKET,
          SO_TYPE,
          (char*) &value,
          &length) == SOCKET_ERROR) {
    switch (WSAGetLastError()) {
      case WSAENOTSOCK:
        return false;
      default:
        // TODO(benh): Handle `WSANOTINITIALISED`.
        ABORT("Not expecting 'getsockopt' to fail when passed a valid socket");
    }
  }

  return true;
}

// NOTE: The below wrappers are used to silence some implicit
// type-casting warnings.

inline int bind(int socket, const struct sockaddr *addr, size_t addrlen)
{
  CHECK_LE(addrlen, INT32_MAX);
  return ::bind(socket, addr, static_cast<int>(addrlen));
}


inline int connect(int socket, const struct sockaddr *address, size_t addrlen)
{
  CHECK_LE(addrlen, INT32_MAX);
  return ::connect(socket, address, static_cast<int>(addrlen));
}


inline ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
  CHECK_LE(len, INT32_MAX);
  return ::send(
      sockfd, static_cast<const char*>(buf), static_cast<int>(len), flags);
}


inline size_t recv(int sockfd, void *buf, size_t len, int flags)
{
  CHECK_LE(len, INT32_MAX);
  return ::recv(sockfd, static_cast<char *>(buf), static_cast<int>(len), flags);
}

} // namespace net {

#endif // __STOUT_OS_WINDOWS_SOCKET_HPP__
