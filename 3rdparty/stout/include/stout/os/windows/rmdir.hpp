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

#ifndef __STOUT_OS_WINDOWS_RMDIR_HPP__
#define __STOUT_OS_WINDOWS_RMDIR_HPP__

#include <string>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/rm.hpp>
#include <stout/os/stat.hpp>

#include <stout/internal/windows/longpath.hpp>

namespace os {
namespace internal {

// Recursive version of `RemoveDirectory`. Two things are notable about this
// implementation:
//
// 1. Unlike `rmdir`, this requires Windows-formatted paths, and therefore
//    should be in the `internal` namespace.
// 2. To match the semantics of the POSIX implementation, this function
//    implements the semantics of `rm -r`, rather than `rmdir`. In particular,
//    if `path` points at a file, this function will delete it, while a call to
//    `rmdir` will not.
inline Try<Nothing> recursive_remove_directory(
    const std::string& path, bool removeRoot, bool continueOnError)
{
  // Base recursion case to delete a symlink or file.
  //
  // We explicitly delete symlinks here to handle hanging symlinks. Note that
  // `os::rm` will correctly delete the symlink, not the target.
  if (os::stat::islink(path) || os::stat::isfile(path)) {
    return os::rm(path);
  }

  // Recursion case to delete all files and subdirectories of a directory.

  // Appending a slash here if the path doesn't already have one simplifies
  // path join logic later, because (unlike Unix) Windows doesn't like double
  // slashes in paths.
  const std::string current_path =
    strings::endsWith(path, "\\") ? path : path + "\\";

  const std::wstring long_current_path =
    ::internal::windows::longpath(current_path);

  // Scope the `search_handle` so that it is closed before we delete the current
  // directory.
  {
    // Get first file matching pattern `X:\path\to\wherever\*`.
    WIN32_FIND_DATAW found;
    const std::wstring search_pattern = long_current_path + L"*";
    const SharedHandle search_handle(
        ::FindFirstFileW(search_pattern.data(), &found), ::FindClose);

    if (search_handle.get() == INVALID_HANDLE_VALUE) {
      return WindowsError(
          "FindFirstFile failed for pattern " + stringify(search_pattern));
    }

    do {
      // NOTE: do-while is appropriate here because folder is guaranteed to have
      // at least a file called `.` (and probably also one called `..`).
      const std::wstring current_file(found.cFileName);

      const bool is_current_directory = current_file.compare(L".") == 0;
      const bool is_parent_directory = current_file.compare(L"..") == 0;

      // Don't try to delete `.` and `..` files in directory.
      if (is_current_directory || is_parent_directory) {
        continue;
      }

      // Path to remove, note that recursion will call `longpath`.
      const std::wstring current_absolute_path =
        long_current_path + current_file;

      // Depth-first search, deleting files and directories.
      Try<Nothing> removed = recursive_remove_directory(
          stringify(current_absolute_path), true, continueOnError);

      if (removed.isError()) {
        if (continueOnError) {
          LOG(WARNING) << "Failed to delete path "
                       << stringify(current_absolute_path) << " with error "
                       << removed.error();
        } else {
          return Error(removed.error());
        }
      }
    } while (::FindNextFileW(search_handle.get(), &found));

    // Check that this loop ended for the right reason.
    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
      return WindowsError(error);
    }
  } // Search Handle is closed when this scope is exited.

  // Finally, remove current directory unless `removeRoot` is disabled.
  if (removeRoot) {
    if (!os::stat::isdir(current_path,
                         os::stat::FollowSymlink::DO_NOT_FOLLOW_SYMLINK)) {
      return Error("Refusing to rmdir non-directory " + current_path);
    } else {
      return os::rm(current_path);
    }
  }

  return Nothing();
}

} // namespace internal {


// By default, recursively deletes a directory akin to: 'rm -r'. If
// `recursive` is false, it deletes a directory akin to: 'rmdir'. In
// recursive mode, `removeRoot` can be set to false to enable removing
// all the files and directories beneath the given root directory, but
// not the root directory itself.
//
// Note that this function expects an absolute path.
//
// By default rmdir aborts when an error occurs during the deletion
// of any file but if continueOnError is set to true, rmdir logs the
// error and continues with the next file.
inline Try<Nothing> rmdir(
    const std::string& directory,
    bool recursive = true,
    bool removeRoot = true,
    bool continueOnError = false)
{
  // The API of this function also deletes files symlinks according
  // to the tests.
  if (!os::exists(directory)) {
    return WindowsError(ERROR_FILE_NOT_FOUND);
  }

  if (recursive) {
    return os::internal::recursive_remove_directory(
        directory, removeRoot, continueOnError);
  } else {
    if (!os::stat::isdir(directory,
                         os::stat::FollowSymlink::DO_NOT_FOLLOW_SYMLINK)) {
      return Error("Refusing to rmdir non-directory " + directory);
    } else {
      return os::rm(directory);
    }
  }
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_RMDIR_HPP__
