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

#include <glog/logging.h>

#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/realpath.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/stat.hpp>

#include <stout/windows/error.hpp>

#include <stout/internal/windows/longpath.hpp>
#include <stout/internal/windows/reparsepoint.hpp>


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
  // NOTE: Special case required to match the semantics of POSIX. See comment
  // above. As below, this also handles symlinks correctly, i.e., given a path
  // to a symlink, we delete the symlink rather than the target.
  if (os::stat::isfile(path)) {
    return os::rm(path);
  }

  // Appending a slash here if the path doesn't already have one simplifies
  // path join logic later, because (unlike Unix) Windows doesn't like double
  // slashes in paths.
  std::string current_path;

  if (!strings::endsWith(path, "\\")) {
    current_path = path + "\\";
  } else {
    current_path = path;
  }
  const std::wstring long_current_path =
      ::internal::windows::longpath(current_path);

  // Get first file matching pattern `X:\path\to\wherever\*`.
  WIN32_FIND_DATAW found;
  const std::wstring search_pattern = long_current_path + L"*";
  const SharedHandle search_handle(
      ::FindFirstFileW(search_pattern.data(), &found),
      ::FindClose);

  if (search_handle.get() == INVALID_HANDLE_VALUE) {
    return WindowsError(
        "`os::internal::recursive_remove_directory` failed when searching "
        "for files with pattern '" + stringify(search_pattern) + "'");
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

    // Path to remove.
    const std::wstring current_absolute_path = long_current_path + current_file;

    Try<bool> is_reparse_point =
      ::internal::windows::reparse_point_attribute_set(current_absolute_path);

    // Delete current path, whether it's a symlink, directory, or file.
    if (!is_reparse_point.isError() && is_reparse_point.get()) {
      // NOTE: This is a best-effort attempt to delete symlinks even when they
      // are "hanging" (i.e., when the target has since been deleted). We call
      // both `RemoveDirectory` and `DeleteFile` here because we are not sure
      // whether the deleted target was a directory or a file, which in general
      // is hard to determine on Windows.
      //
      // If either `RemoveDirectory` or `DeleteFile` succeeds, the reparse
      // point has been successfully removed, and we report success.
      const BOOL rmdir = ::RemoveDirectoryW(current_absolute_path.data());

      if (rmdir == FALSE) {
        const BOOL rm = ::DeleteFileW(current_absolute_path.data());

        if (rm == FALSE) {
          return WindowsError(
              "Failed to remove reparse point at '" +
              stringify(current_absolute_path) + "'");
        }
      }
    } else if (os::stat::isdir(stringify(current_absolute_path))) {
      Try<Nothing> removed = recursive_remove_directory(
          stringify(current_absolute_path), true, continueOnError);

      if (removed.isError()) {
        if (continueOnError) {
          LOG(WARNING) << "Failed to delete directory "
                       << stringify(current_absolute_path)
                       << " with error " << removed.error();
        } else {
          return Error(removed.error());
        }
      }
    } else {
      if (::DeleteFileW(current_absolute_path.data()) == 0) {
        if (continueOnError) {
          LOG(WARNING)
              << "`os::internal::recursive_remove_directory`"
              << " attempted to delete file '"
              << stringify(current_absolute_path) << "', but failed";
        } else {
          return WindowsError(
              "`os::internal::recursive_remove_directory` attempted to delete "
              "file '" + stringify(current_absolute_path) + "', but failed");
        }
      }
    }
  } while (::FindNextFileW(search_handle.get(), &found));

  // Finally, remove current directory unless `removeRoot` is disabled.
  if (removeRoot && ::RemoveDirectoryW(long_current_path.data()) == FALSE) {
    if (continueOnError) {
      LOG(WARNING) << "`os::internal::recursive_remove_directory`"
                   << " attempted to delete directory '"
                   << current_path << "', but failed";
      return ErrnoError("rmdir failed in 'continueOnError' mode");
    } else {
      return ErrnoError(
          "`os::internal::recursive_remove_directory` attempted to delete "
          "directory '" + current_path + "', but failed");
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
// Note that this function expects an absolute path.
// By default rmdir aborts when an error occurs during the deletion of any file
// but if continueOnError is set to true, rmdir logs the error and continues
// with the next file.
inline Try<Nothing> rmdir(
    const std::string& directory,
    bool recursive = true,
    bool removeRoot = true,
    bool continueOnError = false)
{
  // Canonicalize the path to Windows style for the call to
  // `recursive_remove_directory`.
  Result<std::string> root = os::realpath(directory);

  if (root.isError()) {
    return Error(root.error());
  } else if (root.isNone()) {
    return Error(
        "Argument to `os::rmdir` is not a valid directory or file: '" +
        directory + "'");
  }

  if (!recursive) {
    if (::_rmdir(directory.c_str()) < 0) {
      return ErrnoError();
    } else {
      return Nothing();
    }
  } else {
    return os::internal::recursive_remove_directory(
        root.get(),
        removeRoot,
        continueOnError);
  }
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_RMDIR_HPP__
