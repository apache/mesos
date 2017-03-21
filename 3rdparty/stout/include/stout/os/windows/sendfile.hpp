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

#include <errno.h>

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/windows/fd.hpp>

namespace os {

// Returns the amount of bytes written from the input file
// descriptor to the output socket.
// On error, `Try<ssize_t, SocketError>` contains the error.
inline Try<ssize_t, SocketError> sendfile(
    const WindowsFD& s, const WindowsFD& fd, off_t offset, size_t length)
{
  // NOTE: We convert the `offset` here to avoid potential data loss
  // in the type casting and bitshifting below.
  uint64_t offset_ = offset;

  OVERLAPPED from = {
      0,
      0,
      {static_cast<DWORD>(offset_), static_cast<DWORD>(offset_ >> 32)},
      nullptr};

  CHECK_LE(length, MAXDWORD);
  if (TransmitFile(
          s,
          fd,
          static_cast<DWORD>(length),
          0,
          &from,
          nullptr,
          0) == FALSE &&
      (WSAGetLastError() == WSA_IO_PENDING ||
       WSAGetLastError() == ERROR_IO_PENDING)) {
    DWORD sent = 0;
    DWORD flags = 0;

    if (WSAGetOverlappedResult(s, &from, &sent, TRUE, &flags) == TRUE) {
      return sent;
    }
  }

  return SocketError();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SENDFILE_HPP__
