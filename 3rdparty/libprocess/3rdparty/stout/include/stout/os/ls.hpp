/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_LS_HPP__
#define __STOUT_OS_LS_HPP__

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <string>

namespace os {

inline Try<std::list<std::string> > ls(const std::string& directory)
{
  DIR* dir = opendir(directory.c_str());

  if (dir == NULL) {
    return ErrnoError("Failed to opendir '" + directory + "'");
  }

  // Calculate the size for a "directory entry".
  long name_max = fpathconf(dirfd(dir), _PC_NAME_MAX);

  // If we don't get a valid size, check NAME_MAX, but fall back on
  // 255 in the worst case ... Danger, Will Robinson!
  if (name_max == -1) {
    name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
  }

  size_t name_end = (size_t) offsetof(dirent, d_name) + name_max + 1;

  size_t size = (name_end > sizeof(dirent) ? name_end : sizeof(dirent));

  dirent* temp = (dirent*) malloc(size);

  if (temp == NULL) {
    // Preserve malloc error.
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
    errno = error;
    return ErrnoError("Failed to read directories");
  }

  return result;
}

} // namespace os {

#endif // __STOUT_OS_LS_HPP__
