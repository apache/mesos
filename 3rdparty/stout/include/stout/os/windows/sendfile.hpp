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

#ifndef __STOUT_OS_WINDOWS_SENDFILE_HPP__
#define __STOUT_OS_WINDOWS_SENDFILE_HPP__

#include <stout/error.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp> // For `winioctl.h`.

#include <stout/internal/windows/overlapped.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

inline Result<size_t> sendfile_async(
    const int_fd& s, const int_fd& fd, size_t length, OVERLAPPED* overlapped)
{
  // `::TransmitFile` can only send `INT_MAX - 1` bytes.
  CHECK_LE(length, INT_MAX - 1);

  const BOOL result = ::TransmitFile(
      s,                          // Sending socket.
      fd,                         // File to be sent.
      static_cast<DWORD>(length), // Number of bytes to be sent from the file.
      0,                          // Bytes per send. 0 chooses system default.
      overlapped,                 // Overlapped object with file offset.
      nullptr,                    // Data before and after file send.
      0);                         // Flags.

  const WindowsError error;
  if (result == FALSE &&
      (error.code == WSA_IO_PENDING || error.code == ERROR_IO_PENDING)) {
    return None();
  }

  if (result == FALSE) {
    return error;
  }

  return length;
}

// Returns the amount of bytes written from the input file
// descriptor to the output socket.
// On error, `Try<ssize_t, SocketError>` contains the error.
inline Try<ssize_t, SocketError> sendfile(
    const int_fd& s, const int_fd& fd, off_t offset, size_t length)
{
  if (offset < 0) {
    return SocketError(WSAEINVAL);
  }

  // NOTE: We convert the `offset` here to avoid potential data loss
  // in the type casting and bitshifting below.
  const uint64_t offset_ = offset;

  const Try<OVERLAPPED> from_ =
    ::internal::windows::init_overlapped_for_sync_io();

  if (from_.isError()) {
    return SocketError(from_.error());
  }

  OVERLAPPED from = from_.get();
  from.Offset = static_cast<DWORD>(offset_);
  from.OffsetHigh = static_cast<DWORD>(offset_ >> 32);

  const Result<size_t> result = sendfile_async(s, fd, length, &from);
  if (result.isError()) {
    return SocketError(result.error());
  }

  if (result.isSome()) {
    return result.get();
  }

  DWORD sent = 0;
  DWORD flags = 0;
  if (::WSAGetOverlappedResult(s, &from, &sent, TRUE, &flags) == TRUE) {
    return sent;
  }

  return SocketError();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SENDFILE_HPP__
