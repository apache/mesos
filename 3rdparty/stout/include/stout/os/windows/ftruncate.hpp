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

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<Nothing> ftruncate(const int_fd& fd, off_t length)
{
  FILE_END_OF_FILE_INFO info;
  info.EndOfFile.QuadPart = length;
  if (::SetFileInformationByHandle(
          fd, FileEndOfFileInfo, &info, sizeof(info)) == FALSE) {
    return WindowsError();
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_FTRUNCATE_HPP__
