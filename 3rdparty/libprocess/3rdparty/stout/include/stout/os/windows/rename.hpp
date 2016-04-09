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

#ifndef __STOUT_OS_WINDOWS_RENAME_HPP__
#define __STOUT_OS_WINDOWS_RENAME_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>


namespace os {

inline Try<Nothing> rename(const std::string& from, const std::string& to)
{
  // Use `MoveFile` to perform the file move. The MSVCRT implementation of
  // `::rename` fails if the `to` file already exists[1], while some UNIX
  // implementations allow that[2].
  //
  // Use `MOVEFILE_COPY_ALLOWED` to allow moving the file to another volume and
  // `MOVEFILE_REPLACE_EXISTING` to comply with the UNIX implementation and
  // replace an existing file[3].
  //
  // [1] https://msdn.microsoft.com/en-us/library/zw5t957f.aspx
  // [2] http://man7.org/linux/man-pages/man2/rename.2.html
  // [3] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365240(v=vs.85).aspx
  const BOOL result = ::MoveFileEx(
      from.c_str(),
      to.c_str(),
      MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);

  if (!result) {
    return WindowsError(
        "`os::rename` failed to move file '" + from + "' to '" + to + "'");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_RENAME_HPP__
