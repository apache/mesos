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

namespace internal {
namespace windows {

// NOTE: File and directory deletion on Windows is an asynchronous operation,
// and there is no built-in way to "wait" on the deletion. So we wait by
// checking for the path's existence until there is a "file not found" error.
// Until the file is actually deleted, this will loop on an access denied error
// (the file exists but has been marked for deletion).
inline Try<Nothing> wait_on_delete(const std::string& path)
{
  // Try for 1 second in 10 intervals of 100 ms.
  for (int i = 0; i < 10; ++i) {
    // This should always fail if the file has been marked for deletion.
    const DWORD attributes =
      ::GetFileAttributesW(::internal::windows::longpath(path).data());
    CHECK_EQ(attributes, INVALID_FILE_ATTRIBUTES);
    const DWORD error = ::GetLastError();

    if (error == ERROR_ACCESS_DENIED) {
      LOG(WARNING) << "Waiting for file " << path << " to be deleted";
      os::sleep(Milliseconds(100));
    } else if (error == ERROR_FILE_NOT_FOUND) {
      // The file is truly gone, stop waiting.
      return Nothing();
    } else {
      return WindowsError(error);
    }
  }

  return Error("Timed out when waiting for file " + path + " to be deleted");
}

} // namespace windows {
} // namespace internal {

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
    return WindowsError();
  }

  // This wait is necessary because the `RemoveDirectory` API does not know to
  // wait for pending deletions of files in the directory, and can otherwise
  // immediately fail with "directory not empty" if there still exists a marked
  // for deletion but not yet deleted file. By making waiting synchronously, we
  // gain the behavior of the POSIX API.
  Try<Nothing> deleted = ::internal::windows::wait_on_delete(path);
  if (deleted.isError()) {
    return Error("wait_on_delete failed " + deleted.error());
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_RM_HPP__
