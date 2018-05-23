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

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp>

#include <stout/internal/windows/overlapped.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/socket.hpp>

namespace os {

// Asynchronous write on a overlapped int_fd. Returns `Error` on fatal errors,
// `None()` on a successful pending IO operation or number of bytes written on
// a successful IO operation that finished immediately.
inline Result<size_t> write_async(
    const int_fd& fd,
    const void* data,
    size_t size,
    OVERLAPPED* overlapped)
{
  CHECK_LE(size, UINT_MAX);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      DWORD bytes;
      const bool success =
        ::WriteFile(fd, data, static_cast<DWORD>(size), &bytes, overlapped);

      return ::internal::windows::process_async_io_result(success, bytes);
    }
    case WindowsFD::Type::SOCKET: {
      static_assert(
          std::is_same<OVERLAPPED, WSAOVERLAPPED>::value,
          "Expected `WSAOVERLAPPED` to be of type `OVERLAPPED`.");

      // Note that it's okay to allocate this on the stack, since the WinSock
      // providers must copy the WSABUF to their internal buffers. See
      // https://msdn.microsoft.com/en-us/library/windows/desktop/ms741688(v=vs.85).aspx // NOLINT(whitespace/line_length)
      WSABUF buf = {
        static_cast<u_long>(size),
        static_cast<char*>(const_cast<void*>(data))
      };

      DWORD bytes;
      const int result =
        ::WSASend(fd, &buf, 1, &bytes, 0, overlapped, nullptr);

      return ::internal::windows::process_async_io_result(result == 0, bytes);
    }
  }

  UNREACHABLE();
}


inline ssize_t write(const int_fd& fd, const void* data, size_t size)
{
  CHECK_LE(size, INT_MAX);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      // Handle non-overlapped case. We just use the regular `WriteFile` since
      // seekable overlapped files require an offset, which we don't track.
      if (!fd.is_overlapped()) {
        DWORD bytes;
        const BOOL result =
          ::WriteFile(fd, data, static_cast<DWORD>(size), &bytes, nullptr);

        if (result == FALSE) {
          // Indicates an error, but we can't return a `WindowsError`.
          return -1;
        }

        return static_cast<ssize_t>(bytes);
      }

      // Asynchronous handle, we can use the `write_async` function
      // and then wait on the overlapped object for a synchronous write.
      Try<OVERLAPPED> overlapped_ =
        ::internal::windows::init_overlapped_for_sync_io();

      if (overlapped_.isError()) {
        return -1;
      }

      OVERLAPPED overlapped = overlapped_.get();
      Result<size_t> result = write_async(fd, data, size, &overlapped);

      if (result.isError()) {
        return -1;
      }

      if (result.isSome()) {
        return result.get();
      }

      // IO is pending, so wait for the overlapped object.
      DWORD bytes;
      const BOOL wait_success =
        ::GetOverlappedResult(fd, &overlapped, &bytes, TRUE);

      if (wait_success == FALSE) {
        return -1;
      }

      return static_cast<ssize_t>(bytes);
    }
    case WindowsFD::Type::SOCKET: {
      return ::send(fd, (const char*)data, static_cast<int>(size), 0);
    }
  }

  UNREACHABLE();
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_WRITE_HPP__
