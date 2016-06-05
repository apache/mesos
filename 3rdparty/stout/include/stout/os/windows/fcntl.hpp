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

#ifndef __STOUT_OS_WINDOWS_FCNTL_HPP__
#define __STOUT_OS_WINDOWS_FCNTL_HPP__

#include <glog/logging.h>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/socket.hpp>


namespace os {

// NOTE: This is not supported on Windows.
inline Try<Nothing> cloexec(int fd)
{
  LOG(WARNING) << "`os::cloexec` has been called, but is a no-op on Windows";
  return Nothing();
}


// NOTE: This is not supported on Windows.
inline Try<bool> isCloexec(int fd)
{
  LOG(WARNING) << "`os::isCloexec` has been called, but is a stub on Windows";
  return true;
}


inline Try<Nothing> nonblock(int fd)
{
  if (net::is_socket(fd)) {
    const u_long non_block_mode = 1;
    u_long mode = non_block_mode;

    int result = ioctlsocket(fd, FIONBIO, &mode);
    if (result != NO_ERROR) {
      return WindowsSocketError();
    }
  } else {
    // Extract handle from file descriptor.
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE) {
      return WindowsError("Failed to get `HANDLE` for file descriptor");
    }

    if (GetFileType(handle) == FILE_TYPE_PIPE) {
      DWORD pipe_mode = PIPE_NOWAIT;
      if (SetNamedPipeHandleState(handle, &pipe_mode, nullptr, nullptr)) {
        return WindowsError();
      }
    }
  }

  return Nothing();
}


inline Try<Nothing> nonblock(HANDLE handle)
{
  return nonblock(
      _open_osfhandle(reinterpret_cast<intptr_t>(handle), O_RDONLY));
}


// NOTE: This is not supported on Windows.
inline Try<bool> isNonblock(int fd)
{
  LOG(WARNING) << "`os::isNonblock` has been called, but is a stub on Windows";
  return true;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_FCNTL_HPP__
