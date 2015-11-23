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

#include <stout/windows.hpp>

#include <stout/os/realpath.hpp>


namespace os {


inline bool exists(const std::string& path)
{
  Result<std::string> absolutePath = os::realpath(path);

  if (!absolutePath.isSome()) {
    return false;
  }

  // NOTE: GetFileAttributes does not support unicode natively. See also
  // "documentation"[1] for why this is a check-if-file-exists idiom.
  //
  // [1] http://blogs.msdn.com/b/oldnewthing/archive/2007/10/23/5612082.aspx
  DWORD attributes = GetFileAttributes(absolutePath.get().c_str());

  bool fileNotFound = GetLastError() == ERROR_FILE_NOT_FOUND;

  return !((attributes == INVALID_FILE_ATTRIBUTES) && fileNotFound);
}


// Determine if the process identified by pid exists.
// NOTE: Zombie processes have a pid and therefore exist. See
// os::process(pid) to get details of a process.
inline bool exists(pid_t pid)
{
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

  // NOTE: `GetExitCode` will gracefully deal with the case that `handle` is
  // `NULL`.
  DWORD exitCode = 0;
  BOOL exitCodeExists = GetExitCodeProcess(handle, &exitCode);

  // `CloseHandle`, on the other hand, will throw an exception in the
  // VS debugger if you pass it a broken handle. (cf. "Return value"
  // section of the documentation[1].)
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms724211(v=vs.85).aspx
  if (handle != NULL) {
    CloseHandle(handle);
  }

  // NOTE: Windows quirk, the exit code returned by the process can
  // be the same number as `STILL_ACTIVE`, in which case this
  // function will mis-report that the process still exists.
  return exitCodeExists && (exitCode == STILL_ACTIVE);
}


} // namespace os {

#endif // __STOUT_OS_WINDOWS_EXISTS_HPP__
