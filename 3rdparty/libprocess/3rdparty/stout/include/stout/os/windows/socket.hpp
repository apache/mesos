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

#include <stout/abort.hpp>

namespace net {

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

} // namespace net {

#endif // __STOUT_OS_WINDOWS_SOCKET_HPP__
