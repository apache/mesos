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

#ifndef __STOUT_OS_POSIX_LSOF_HPP__
#define __STOUT_OS_POSIX_LSOF_HPP__

#include <string>
#include <vector>

#include <stout/numify.hpp>
#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/ls.hpp>

namespace os {

// Get all the open file descriptors of the current process.
inline Try<std::vector<int_fd>> lsof()
{
  int fdDir = ::open("/dev/fd", O_RDONLY | O_CLOEXEC);
  if (fdDir == -1) {
    return ErrnoError("Failed to open '/dev/fd'");
  }

  DIR* dir = ::fdopendir(fdDir);
  if (dir == nullptr) {
    Error error = ErrnoError("Failed to fdopendir '/dev/fd'");
    ::close(fdDir);
    return error;
  }

  struct dirent* entry;
  std::vector<int_fd> result;

  // Zero `errno` before starting to call `readdir`. This is necessary
  // to allow us to determine when `readdir` returns an error.
  errno = 0;

  while ((entry = ::readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    Try<int_fd> fd = numify<int_fd>(entry->d_name);
    if (fd.isError()) {
      return Error(
          "Could not interpret file descriptor '" +
          std::string(entry->d_name) + "': " + fd.error());
    }

    if (fd.get() != fdDir) {
      result.push_back(fd.get());
    }
  }

  if (errno != 0) {
    // Preserve `readdir` error.
    Error error = ErrnoError("Failed to read directory");
    ::closedir(dir);
    return error;
  }

  if (::closedir(dir) == -1) {
    return ErrnoError("Failed to close directory");
  }

  return result;
}

} // namespace os {

#endif /* __STOUT_OS_POSIX_LSOF_HPP__  */
