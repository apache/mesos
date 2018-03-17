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

#ifndef __STOUT_OS_WINDOWS_LSEEK_HPP__
#define __STOUT_OS_WINDOWS_LSEEK_HPP__

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<off_t> lseek(int_fd fd, off_t offset, int whence)
{
  // NOTE: The values for `SEEK_SET`, `SEEK_CUR`, and `SEEK_END` are
  // 0, 1, 2, the same as `FILE_BEGIN`, `FILE_CURRENT`, and
  // `FILE_END`. Thus we don't need to map them, and they can be
  // casted to a `DWORD` safely.

  LARGE_INTEGER offset_;
  offset_.QuadPart = offset;

  LARGE_INTEGER new_offset;

  // TODO(andschwa): This may need to be synchronized if users aren't
  // careful about sharing their file handles among threads.
  const BOOL result =
    ::SetFilePointerEx(fd, offset_, &new_offset, static_cast<DWORD>(whence));

  if (result == FALSE) {
    return WindowsError();
  }

  return static_cast<off_t>(new_offset.QuadPart);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_LSEEK_HPP__
