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

#ifndef __STOUT_OS_WINDOWS_OPEN_HPP__
#define __STOUT_OS_WINDOWS_OPEN_HPP__

#include <fcntl.h> // For file access flags like `_O_CREAT`.

#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp> // For `mode_t`.

#include <stout/os/int_fd.hpp>

#include <stout/internal/windows/longpath.hpp>

// TODO(andschwa): Windows does not support the Linux extension
// O_NONBLOCK, as asynchronous I/O is done through other mechanisms.
// Overlapped I/O will be implemented later.
constexpr int O_NONBLOCK = 0;

// Windows does not support the Linux extension O_SYNC, as buffering
// is done differently.
// TODO(andschwa): This could be equivalent to
// `FILE_FLAG_WRITE_THROUGH`, but we don't seem to need it.
constexpr int O_SYNC = 0;

// Windows does not support the Linux extension O_CLOEXEC. Instead, by
// default we set all handles to be non-inheritable.
constexpr int O_CLOEXEC = 0;

namespace os {

// TODO(andschwa): Handle specified creation permissions in `mode_t mode`. See
// MESOS-3176.
//
// NOTE: This function always opens files in non-overlapped mode, because
// we only support overlapped pipes and sockets through the `os::pipe`
// and `os::socket` functions.
inline Try<int_fd> open(const std::string& path, int oflag, mode_t mode = 0)
{
  std::wstring longpath = ::internal::windows::longpath(path);

  // Map the POSIX `oflag` access flags.

  // O_APPEND: Write only appends.
  //
  // NOTE: We choose a `write` flag here because emulating `O_APPEND`
  // requires granting the `FILE_APPEND_DATA` access right, but not
  // the `FILE_WRITE_DATA` access right, which `GENERIC_WRITE` would
  // otherwise grant.
  const DWORD write = (oflag & O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE;

  DWORD access;
  switch (oflag & (O_RDONLY | O_WRONLY | O_RDWR)) {
    case O_RDONLY: {
      access = GENERIC_READ;
      break;
    }
    case O_WRONLY: {
      access = write;
      break;
    }
    case O_RDWR: {
      access = GENERIC_READ | write;
      break;
    }
    default: {
      return Error("Access mode not specified.");
    }
  }

  // Map the POSIX `oflag` creation flags.
  DWORD create;
  switch (oflag & (O_CREAT | O_EXCL | O_TRUNC)) {
    case O_CREAT: {
      // Create a new file or open an existing file.
      create = OPEN_ALWAYS;
      break;
    }
    case O_CREAT | O_EXCL:
    case O_CREAT | O_EXCL | O_TRUNC: {
      // Create a new file, but fail if it already exists.
      // Ignore `O_TRUNC` with `O_CREAT | O_EXCL`
      create = CREATE_NEW;
      break;
    }
    case O_CREAT | O_TRUNC: {
      // Truncate file if it already exists.
      create = CREATE_ALWAYS;
      break;
    }
    case O_EXCL:
    case O_EXCL | O_TRUNC: {
      return Error("`O_EXCL` is undefined without `O_CREAT`.");
    }
    case O_TRUNC: {
      // Truncate file if it exists, otherwise fail.
      create = TRUNCATE_EXISTING;
      break;
    }
    default: {
      // Open file if it exists, otherwise fail.
      create = OPEN_EXISTING;
      break;
    }
  }

  const HANDLE handle = ::CreateFileW(
      longpath.data(),
      access,
      // Share all access so we don't lock the file.
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      // Disable inheritance by default.
      nullptr,
      create,
      FILE_ATTRIBUTE_NORMAL,
      // No template file.
      nullptr);

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  return handle;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_OPEN_HPP__
