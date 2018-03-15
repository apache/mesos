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

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>  // For `mode_t`.

#include <stout/os/close.hpp>
#include <stout/os/fcntl.hpp> // For `oflag` values.
#include <stout/os/int_fd.hpp>

#include <stout/internal/windows/longpath.hpp>

#ifndef O_CLOEXEC
#error "missing O_CLOEXEC support on this platform"
// NOTE: On Windows, `fnctl.hpp` defines `O_CLOEXEC` to a no-op.
#endif

namespace os {

inline Try<int_fd> open(const std::string& path, int oflag, mode_t mode = 0)
{
  std::wstring longpath = ::internal::windows::longpath(path);
  // By default, Windows will perform "text translation" meaning that it will
  // automatically write CR/LF instead of LF line feeds. To prevent this, and
  // use the POSIX semantics, we open with `O_BINARY`.
  //
  // Also by default, we will mimic the Windows (non-CRT) APIs and make all
  // opened handles non-inheritable.
  int_fd fd = ::_wopen(longpath.data(), oflag | O_BINARY | O_NOINHERIT, mode);
  if (fd < 0) {
    return ErrnoError();
  }

  return fd;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_OPEN_HPP__
