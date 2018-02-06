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

#ifndef __STOUT_WINDOWS_FS_HPP__
#define __STOUT_WINDOWS_FS_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/realpath.hpp>

#include <stout/internal/windows/longpath.hpp>
#include <stout/internal/windows/symlink.hpp>

namespace fs {

// Returns the total disk size in bytes.
inline Try<Bytes> size(const std::string& path = "/")
{
  Result<std::string> real_path = os::realpath(path);
  if (!real_path.isSome()) {
    return Error(
        "Failed to get realpath for '" + path+ "': " +
        (real_path.isError() ? real_path.error() : "No such directory"));
  }

  ULARGE_INTEGER free_bytes, total_bytes, total_free_bytes;
  if (::GetDiskFreeSpaceExW(
          internal::windows::longpath(real_path.get()).data(),
          &free_bytes,
          &total_bytes,
          &total_free_bytes) == 0) {
    return WindowsError(
        "Error invoking 'GetDiskFreeSpaceEx' on '" + path + "'");
  }

  return Bytes(total_bytes.QuadPart);
}


// Returns relative disk usage of the file system that the given path
// is mounted at.
inline Try<double> usage(const std::string& path = "/")
{
  Result<std::string> real_path = os::realpath(path);
  if (!real_path.isSome()) {
    return Error(
        "Failed to get realpath for '" + path + "': " +
        (real_path.isError() ? real_path.error() : "No such directory"));
  }

  ULARGE_INTEGER free_bytes, total_bytes, total_free_bytes;
  if (::GetDiskFreeSpaceExW(
          internal::windows::longpath(real_path.get()).data(),
          &free_bytes,
          &total_bytes,
          &total_free_bytes) == 0) {
    return WindowsError(
        "Error invoking 'GetDiskFreeSpaceEx' on '" + path + "'");
  }

  double used = static_cast<double>(total_bytes.QuadPart - free_bytes.QuadPart);
  return used / total_bytes.QuadPart;
}


inline Try<Nothing> symlink(
    const std::string& original,
    const std::string& link)
{
  return internal::windows::create_symbolic_link(original, link);
}


// Returns a list of all files matching the given pattern. This is meant to
// be a lightweight alternative to glob() - the only supported wildcards are
// `?` and `*`, and only when they appear at the tail end of `pattern` (e.g.
// `/root/dir/subdir/*.txt` or `/root/dir/subdir/file?.txt`.
inline Try<std::list<std::string>> list(const std::string& pattern)
{
  const std::string dirname(Path(pattern).dirname());
  std::list<std::string> found_files;
  WIN32_FIND_DATAW found;
  const SharedHandle search_handle(
    ::FindFirstFileW(wide_stringify(pattern).data(), &found),
    ::FindClose);

  if (search_handle.get() == INVALID_HANDLE_VALUE) {
    // For compliance with the POSIX implementation (which uses `::glob`),
    // return an empty list instead of an error when the path does not exist.
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return found_files;
    }

    return WindowsError(error, "FindFirstFile failed");
  }

  do {
    const std::wstring current_file(found.cFileName);

    // Ignore `.` and `..` entries.
    if (current_file.compare(L".") != 0 && current_file.compare(L"..") != 0) {
      found_files.push_back(path::join(dirname, stringify(current_file)));
    }
  } while (::FindNextFileW(search_handle.get(), &found));

  const DWORD error = ::GetLastError();
  if (error != ERROR_NO_MORE_FILES) {
    return WindowsError(error, "FindNextFile failed");
  }

  return found_files;
}

} // namespace fs {

#endif // __STOUT_WINDOWS_FS_HPP__
