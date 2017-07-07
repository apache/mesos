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
#ifndef __STOUT_OS_WINDOWS_RM_HPP__
#define __STOUT_OS_WINDOWS_RM_HPP__

#include <stdio.h>

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/stat.hpp>

#include <stout/internal/windows/longpath.hpp>


namespace os {

inline Try<Nothing> rm(const std::string& path)
{
  // This function uses either `RemoveDirectory` or `DeleteFile` to remove the
  // actual filesystem entry. These WinAPI functions are being used instead of
  // the POSIX `remove` because their behavior when it comes to symbolic links
  // is well documented in MSDN[1][2]. Whenever called on a symbolic link, both
  // `RemoveDirectory` and `DeleteFile` act on the symlink itself, rather than
  // its target.
  //
  // Because `RemoveDirectory` fails if the specified path is not an empty
  // directory, its behavior is consistent with the POSIX[3] `remove`[3] (which
  // uses `rmdir`[4]).
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365488(v=vs.85).aspx
  // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365682(v=vs.85).aspx#deletefile_and_deletefiletransacted
  // [3] http://pubs.opengroup.org/onlinepubs/009695399/functions/remove.html
  // [4] http://pubs.opengroup.org/onlinepubs/009695399/functions/rmdir.html
  const std::wstring longpath = ::internal::windows::longpath(path);
  const BOOL result = os::stat::isdir(path)
      ? ::RemoveDirectoryW(longpath.data())
      : ::DeleteFileW(longpath.data());

  if (!result) {
    return WindowsError("`os::rm` could not remove '" + path + "'");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_RM_HPP__
