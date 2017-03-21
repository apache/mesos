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

#ifndef __STOUT_OS_WINDOWS_FTRUNCATE_HPP__
#define __STOUT_OS_WINDOWS_FTRUNCATE_HPP__

#include <io.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/windows/fd.hpp>


namespace os {

// Identical in functionality to POSIX standard `ftruncate`.
inline Try<Nothing> ftruncate(const WindowsFD& fd, __int64 length)
{
  if (::_chsize_s(fd.crt(), length) != 0) {
    return ErrnoError(
      "Failed to truncate file at file descriptor '" + stringify(fd) + "' to " +
      stringify(length) + " bytes.");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_FTRUNCATE_HPP__
