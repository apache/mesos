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

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp> // For `WinSock2.h`.

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<int_fd> dup(const int_fd& fd)
{
  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      HANDLE duplicate = INVALID_HANDLE_VALUE;
      const BOOL result = ::DuplicateHandle(
          ::GetCurrentProcess(),  // Source process == current.
          fd,                     // Handle to duplicate.
          ::GetCurrentProcess(),  // Target process == current.
          &duplicate,
          0,                      // Ignored (DUPLICATE_SAME_ACCESS).
          FALSE,                  // Non-inheritable handle.
          DUPLICATE_SAME_ACCESS); // Same access level as source.

      if (result == FALSE) {
        return WindowsError();
      }

      WindowsFD dup_fd(fd);
      dup_fd.handle_ = duplicate;
      return dup_fd;
    }
    case WindowsFD::Type::SOCKET: {
      WSAPROTOCOL_INFOW info;
      const int result =
        ::WSADuplicateSocketW(fd, ::GetCurrentProcessId(), &info);
      if (result != 0) {
        return SocketError();
      }

      SOCKET duplicate = ::WSASocketW(0, 0, 0, &info, 0, 0);
      if (duplicate == INVALID_SOCKET) {
        return WindowsSocketError();
      }

      WindowsFD dup_fd(fd);
      dup_fd.socket_ = duplicate;
      return dup_fd;
    }
  }

  UNREACHABLE();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_DUP_HPP__
