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

#ifndef __STOUT_OS_LS_HPP__
#define __STOUT_OS_LS_HPP__

#ifdef __WINDOWS__
#include <stout/internal/windows/dirent.hpp>
#else
#include <dirent.h>
#endif // __WINDOWS__

#include <errno.h>
#include <stdlib.h>

#include <list>
#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>


namespace os {

inline Try<std::list<std::string>> ls(const std::string& directory)
{
  DIR* dir = opendir(directory.c_str());

  if (dir == nullptr) {
    return ErrnoError("Failed to opendir '" + directory + "'");
  }

  std::list<std::string> result;
  struct dirent* entry;

  // Zero `errno` before starting to call `readdir`. This is necessary
  // to allow us to determine when `readdir` returns an error.
  errno = 0;

  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    result.push_back(entry->d_name);
  }

  if (errno != 0) {
    // Preserve `readdir` error.
    Error error = ErrnoError("Failed to read directory");
    closedir(dir);
    return error;
  }

  if (closedir(dir) == -1) {
    return ErrnoError("Failed to close directory");
  }

  return result;
}

} // namespace os {

#endif // __STOUT_OS_LS_HPP__
