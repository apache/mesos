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
#include <stdlib.h>

#include <list>
#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>

#include <stout/os/direntsize.hpp>


namespace os {

inline Try<std::list<std::string>> ls(const std::string& directory)
{
  DIR* dir = opendir(directory.c_str());

  if (dir == NULL) {
    // Preserve `opendir` error.
    return ErrnoError("Failed to opendir '" + directory + "'");
  }

  dirent* temp = (dirent*) malloc(os::dirent_size(dir));

  if (temp == NULL) {
    // Preserve `malloc` error.
    ErrnoError error("Failed to allocate directory entries");
    closedir(dir);
    return error;
  }

  std::list<std::string> result;
  struct dirent* entry;
  int error;

  while ((error = readdir_r(dir, temp, &entry)) == 0 && entry != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    result.push_back(entry->d_name);
  }

  free(temp);
  closedir(dir);

  if (error != 0) {
    // Preserve `readdir_r` error.
    return ErrnoError("Failed to read directories");
  }

  return result;
}

} // namespace os {

#endif // __STOUT_OS_LS_HPP__
