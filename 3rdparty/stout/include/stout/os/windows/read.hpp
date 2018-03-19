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

#ifndef __STOUT_OS_WINDOWS_READ_HPP__
#define __STOUT_OS_WINDOWS_READ_HPP__

#include <stout/result.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/socket.hpp>

namespace os {

inline ssize_t read(const int_fd& fd, void* data, size_t size)
{
  CHECK_LE(size, UINT_MAX);

  switch (fd.type()) {
    // TODO(andschwa): Remove this when `FD_CRT` is removed, MESOS-8675.
    case WindowsFD::FD_CRT: {
      return ::_read(fd.crt(), data, static_cast<unsigned int>(size));
    }
    case WindowsFD::FD_HANDLE: {
      DWORD bytes;
      // TODO(andschwa): Handle overlapped I/O.
      const BOOL result =
        ::ReadFile(fd, data, static_cast<DWORD>(size), &bytes, nullptr);
      if (result == FALSE) {
        // The pipe "breaks" when the other process closes its handle, but we
        // still have the data and therefore do not want to return an error.
        if (::GetLastError() != ERROR_BROKEN_PIPE) {
          // Indicates an error, but we can't return a `WindowsError`.
          return -1;
        }
      }

      return static_cast<ssize_t>(bytes);
    }
    case WindowsFD::FD_SOCKET: {
      return ::recv(fd, (char*)data, static_cast<unsigned int>(size), 0);
    }
  }

  UNREACHABLE();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_READ_HPP__
