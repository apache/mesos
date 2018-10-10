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

#ifndef __STOUT_OS_POSIX_RENAME_HPP__
#define __STOUT_OS_POSIX_RENAME_HPP__

#include <stdio.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/os/fsync.hpp>

namespace os {

// Rename a given path to another one. If `sync` is set to true, `fsync()` will
// be called on both the source directory and the destination directory to
// ensure that the result is committed to their filesystems.
//
// NOTE: This function can fail with `sync` set to true if either the source
// directory or the destination directory gets removed before it returns. If
// multiple processes or threads access to the filesystems concurrently, the
// caller should either enforce a proper synchronization, or set `sync` to false
// and call `fsync()` explicitly on POSIX systems to handle such failures.
inline Try<Nothing> rename(
    const std::string& from,
    const std::string& to,
    bool sync = false)
{
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return ErrnoError();
  }

  if (sync) {
    const std::string to_dir = Path(to).dirname();
    const std::string from_dir = Path(from).dirname();

    std::vector<std::string> dirs = {to_dir};
    if (from_dir != to_dir) {
      dirs.emplace_back(from_dir);
    }

    foreach (const std::string& dir, dirs) {
      Try<Nothing> fsync = os::fsync(dir);

      if (fsync.isError()) {
        return Error(
            "Failed to fsync directory '" + dir + "': " + fsync.error());
      }
    }
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_RENAME_HPP__
