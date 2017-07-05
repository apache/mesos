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

#ifndef __STOUT_OS_WINDOWS_EXISTS_HPP__
#define __STOUT_OS_WINDOWS_EXISTS_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/windows.hpp>

#include <stout/internal/windows/longpath.hpp>


namespace os {


inline bool exists(const std::string& path)
{
  // NOTE: `GetFileAttributes` returns `INVALID_FILE_ATTRIBUTES` if the file
  // could not be opened for any reason. Checking for one of two 'not found'
  // error codes (`ERROR_FILE_NOT_FOUND` or `ERROR_PATH_NOT_FOUND`) is a
  // reliable test for whether the file or directory exists. See also [1] for
  // more information on this technique.
  //
  // [1] http://blogs.msdn.com/b/oldnewthing/archive/2007/10/23/5612082.aspx
  const DWORD attributes = ::GetFileAttributesW(
      ::internal::windows::longpath(path).data());

  if (attributes == INVALID_FILE_ATTRIBUTES) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return false;
    }
  }

  return true;
}


// Determine if the process identified by pid exists.
// NOTE: Zombie processes have a pid and therefore exist. See
// os::process(pid) to get details of a process.
inline bool exists(pid_t pid)
{
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

  bool has_handle = false;

  if (handle != nullptr) {
    has_handle = true;
    CloseHandle(handle);
  }

  return has_handle;
}


} // namespace os {

#endif // __STOUT_OS_WINDOWS_EXISTS_HPP__
