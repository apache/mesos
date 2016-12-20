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

#ifndef __STOUT_OS_WINDOWS_WRITE_HPP__
#define __STOUT_OS_WINDOWS_WRITE_HPP__

#include <io.h>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp> // For order-dependent networking headers.

#include <stout/os/socket.hpp>
#include <stout/os/windows/fd.hpp>


namespace os {

inline ssize_t write(const WindowsFD& fd, const void* data, size_t size)
{
  CHECK_LE(size, INT_MAX);

  switch (fd.type()) {
    case WindowsFD::FD_CRT:
    case WindowsFD::FD_HANDLE: {
      return ::_write(fd.crt(), data, static_cast<unsigned int>(size));
    }
    case WindowsFD::FD_SOCKET: {
      return ::send(fd, (const char*)data, static_cast<int>(size), 0);
    }
  }

  UNREACHABLE();
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_WRITE_HPP__
