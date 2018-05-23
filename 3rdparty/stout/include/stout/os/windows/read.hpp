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

#include <stout/internal/windows/overlapped.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/socket.hpp>

namespace os {

// Asynchronous read on a overlapped int_fd. Returns `Error` on fatal errors,
// `None()` on a successful pending IO operation or number of bytes read on a
// successful IO operation that finished immediately.
inline Result<size_t> read_async(
    const int_fd& fd,
    void* data,
    size_t size,
    OVERLAPPED* overlapped)
{
  CHECK_LE(size, UINT_MAX);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      DWORD bytes;
      const bool success =
        ::ReadFile(fd, data, static_cast<DWORD>(size), &bytes, overlapped);

      // On failure, there are two EOF cases for reads:
      //   1) ERROR_BROKEN_PIPE: The write end is closed and there is no data.
      //   2) ERROR_HANDLE_EOF: We hit the EOF for an asynchronous file handle.
      const DWORD errorCode = ::GetLastError();
      if (success == FALSE &&
          (errorCode == ERROR_BROKEN_PIPE || errorCode == ERROR_HANDLE_EOF)) {
        return 0;
      }

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
        static_cast<char*>(data)
      };

      DWORD bytes;
      DWORD flags = 0;
      const int result =
        ::WSARecv(fd, &buf, 1, &bytes, &flags, overlapped, nullptr);

      return ::internal::windows::process_async_io_result(result == 0, bytes);
    }
  }

  UNREACHABLE();
}


// Synchronous reads on any int_fd. Returns -1 on error and
// number of bytes read on success.
inline ssize_t read(const int_fd& fd, void* data, size_t size)
{
  CHECK_LE(size, UINT_MAX);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      // Handle non-overlapped case. We just use the regular `ReadFile` since
      // seekable overlapped files require an offset, which we don't track.
      if (!fd.is_overlapped()) {
        DWORD bytes;
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

      // Asynchronous handle, we can use the `read_async` function
      // and then wait on the overlapped object for a synchronous read.
      Try<OVERLAPPED> overlapped_ =
        ::internal::windows::init_overlapped_for_sync_io();

      if (overlapped_.isError()) {
        return -1;
      }

      OVERLAPPED overlapped = overlapped_.get();
      Result<size_t> result = read_async(fd, data, size, &overlapped);

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

      if (wait_success == TRUE) {
        return bytes;
      }

      // On failure, there are two EOF cases for reads:
      //   1) ERROR_BROKEN_PIPE: The write end is closed and there is no data.
      //   2) ERROR_HANDLE_EOF: We hit the EOF for an asynchronous file handle.
      const DWORD error = ::GetLastError();
      if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
        return 0;
      }

      return -1;
    }
    case WindowsFD::Type::SOCKET: {
      return ::recv(fd, (char*)data, static_cast<unsigned int>(size), 0);
    }
  }

  UNREACHABLE();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_READ_HPP__
