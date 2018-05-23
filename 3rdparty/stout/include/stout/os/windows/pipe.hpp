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

#ifndef __STOUT_OS_WINDOWS_PIPE_HPP__
#define __STOUT_OS_WINDOWS_PIPE_HPP__

#include <array>
#include <string>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>
#include <stout/windows.hpp> // For `windows.h`.

#include <stout/os/int_fd.hpp>

namespace os {

// Returns an "anonymous" pipe that can be used for interprocess communication.
// Since anonymous pipes do not support overlapped IO, we emulate it with an
// uniquely named pipe.
//
// NOTE: Overlapped pipes passed to child processes behave weirdly, so we have
// the ability to create non overlapped pipe handles.
inline Try<std::array<int_fd, 2>> pipe(
    bool read_overlapped = true, bool write_overlapped = true)
{
  const DWORD read_flags = read_overlapped ? FILE_FLAG_OVERLAPPED : 0;
  const DWORD write_flags = write_overlapped ? FILE_FLAG_OVERLAPPED : 0;
  std::wstring name =
    wide_stringify("\\\\.\\pipe\\mesos-pipe-" + id::UUID::random().toString());

  // The named pipe name must be at most 256 characters [1]. It doesn't say if
  // it includes the null terminator, so we limit to 255 to be safe. Since the
  // UUID name is fixed length, this is just a sanity check.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365150(v=vs.85).aspx // NOLINT(whitespace/line_length)
  CHECK_LE(name.size(), 255);

  // Create the named pipe. To avoid the time-of-check vs time-of-use attack,
  // we have the `FILE_FLAG_FIRST_PIPE_INSTANCE` flag to fail if the pipe
  // already exists.
  //
  // TODO(akagup): The buffer size is currently required, since we don't have
  // IOCP yet. When testing IOCP, it should be 0, but after that, we can try
  // restoring the buffer for an optimization.
  const HANDLE read_handle = ::CreateNamedPipeW(
      name.data(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | read_flags,
      PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,        // Max pipe instances.
      4096,     // Inbound buffer size.
      4096,     // Outbound buffer size.
      0,        // Pipe timeout for connections.
      nullptr); // Security attributes (not inheritable).

  if (read_handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  // To create a client named pipe, we use the generic `CreateFile` API. We
  // don't use the other named pipe APIs, since they are used for different
  // purposes. For example, `ConnectNamedPipe` is similar to a server calling
  // `accept` for sockets and `CallNamedPipe` is used for message type pipes,
  // but we want a byte stream pipe.
  //
  // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa365598%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396 // NOLINT(whitespace/line_length)
  const HANDLE write_handle = ::CreateFileW(
      name.data(),
      GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | write_flags,
      nullptr);

  const WindowsError error;
  if (write_handle == INVALID_HANDLE_VALUE) {
    ::CloseHandle(read_handle);
    return error;
  }

  return std::array<int_fd, 2>{int_fd(read_handle, read_overlapped),
                               int_fd(write_handle, write_overlapped)};
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_PIPE_HPP__
