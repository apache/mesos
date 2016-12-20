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

#ifndef __STOUT_OS_WINDOWS_CLOSE_HPP__
#define __STOUT_OS_WINDOWS_CLOSE_HPP__

#include <errno.h>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows/error.hpp>

#include <stout/os/windows/fd.hpp>
#include <stout/os/windows/socket.hpp>

namespace os {

inline Try<Nothing> close(const WindowsFD& fd)
{
  switch (fd.type()) {
    case WindowsFD::FD_CRT:
    case WindowsFD::FD_HANDLE: {
      // We don't need to call `CloseHandle` on `fd.handle`, because calling
      // `_close` on the corresponding CRT FD implicitly invokes `CloseHandle`.
      if (::_close(fd.crt()) < 0) {
        return ErrnoError();
      }
      break;
    }
    case WindowsFD::FD_SOCKET: {
      // NOTE: Since closing an unconnected socket is not an error in POSIX,
      // we simply ignore it here.
      if (::shutdown(fd, SD_BOTH) == SOCKET_ERROR &&
          WSAGetLastError() != WSAENOTCONN) {
        return WindowsSocketError("Failed to shutdown a socket");
      }
      if (::closesocket(fd) == SOCKET_ERROR) {
        return WindowsSocketError("Failed to close a socket");
      }
      break;
    }
  }
  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_CLOSE_HPP__
