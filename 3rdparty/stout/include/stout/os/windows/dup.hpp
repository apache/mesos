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

#ifndef __STOUT_OS_WINDOWS_DUP_HPP__
#define __STOUT_OS_WINDOWS_DUP_HPP__

#include <io.h>
#include <Winsock2.h>

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/windows/fd.hpp>

namespace os {

inline Try<WindowsFD> dup(const WindowsFD& fd)
{
  switch (fd.type()) {
    case WindowsFD::FD_CRT:
    case WindowsFD::FD_HANDLE: {
      int result = ::_dup(fd.crt());
      if (result == -1) {
        return ErrnoError();
      }
      return result;
    }
    case WindowsFD::FD_SOCKET: {
#pragma warning(push)
#pragma warning(disable : 4996)
      // Disable compiler warning asking us to use the Unicode version of
      // `WSASocket` and `WSADuplicateSocket`, because Mesos currently does not
      // support Unicode. See MESOS-6817.
      WSAPROTOCOL_INFO protInfo;
      if (::WSADuplicateSocket(fd, GetCurrentProcessId(), &protInfo) !=
          INVALID_SOCKET) {
        return WSASocket(0, 0, 0, &protInfo, 0, 0);
      };
#pragma warning(pop)
      return SocketError();
    }
  }
  UNREACHABLE();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_DUP_HPP__
