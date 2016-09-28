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
#include <stout/windows.hpp> // For order-dependent networking headers.

#include <stout/os/socket.hpp>


namespace os {

// Forward declaration for an OS-agnostic `write`.
inline Try<Nothing> write(int fd, const std::string& message);


inline ssize_t write(int fd, const void* data, size_t size)
{
  CHECK_LE(size, INT_MAX);

  if (net::is_socket(fd)) {
    return net::send(fd, (const char*) data, size, 0);
  }

  return ::_write(fd, data, static_cast<unsigned int>(size));
}


inline ssize_t read(HANDLE handle, const void* data, size_t size)
{
  return ::os::write(
      _open_osfhandle(reinterpret_cast<intptr_t>(handle), O_RDWR),
      data,
      size);
}


inline Try<Nothing> write(HANDLE handle, const std::string& message)
{
  return ::os::write(
      _open_osfhandle(reinterpret_cast<intptr_t>(handle), O_RDWR),
      message);
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_WRITE_HPP__
