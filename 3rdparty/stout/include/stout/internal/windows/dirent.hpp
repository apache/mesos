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

#ifndef __STOUT_INTERNAL_WINDOWS_DIRENT_HPP__
#define __STOUT_INTERNAL_WINDOWS_DIRENT_HPP__

#include <assert.h>
#include <malloc.h>

#include <stout/windows.hpp>


// Abbreviated version of the POSIX `dirent` struct. cf. specification[1].
//
// [1] http://www.gnu.org/software/libc/manual/html_node/Directory-Entries.html
struct dirent
{
  char d_name[MAX_PATH];
  unsigned short d_namlen;
};


// `DIR` is normally an opaque struct in the standard, we expose the
// implementation here because this header is intended for internal use only.
struct DIR
{
  struct dirent curr;
  char *d_name;
  WIN32_FIND_DATA fd;
  HANDLE handle;
};


// Avoid the C++-style name-mangling linkage, and use C-style instead to give
// the appearance that this code is part of the real C standard library.
extern "C" {
namespace internal {

void free_dir(DIR* directory);

bool open_dir_stream(DIR* directory);

bool reentrant_advance_dir_stream(DIR* directory);

} // namespace internal {


// Windows implementation of POSIX standard `opendir`. cf. specification[1].
//
// [1] http://www.gnu.org/software/libc/manual/html_node/Opening-a-Directory.html#Opening-a-Directory
inline DIR* opendir(const char* path)
{
  if (path == nullptr) {
    errno = ENOTDIR;
    return nullptr;
  }

  const size_t path_size = strlen(path);

  if (path_size == 0 || path_size >= MAX_PATH) {
    errno = ENOENT;
    return nullptr;
  }

  const char windows_folder_separator = '\\';
  const char windows_drive_separator = ':';
  const char wildcard[] = "*";
  const char dir_separator_and_wildcard[] = "\\*";

  // Allocate space for directory. Be sure to leave room at the end of
  // `directory->d_name` for a directory separator and a wildcard.
  DIR* directory = (DIR*) malloc(sizeof(DIR));

  if (directory == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }

  directory->d_name =
    (char*) malloc(path_size + strlen(dir_separator_and_wildcard) + 1);

  if (directory->d_name == nullptr) {
    errno = ENOMEM;
    free(directory);
    return nullptr;
  }

  // Copy path over and append the appropriate postfix.
  strcpy(directory->d_name, path);

  const size_t last_char_in_name =
    directory->d_name[strlen(directory->d_name) - 1];

  if (last_char_in_name != windows_folder_separator &&
      last_char_in_name != windows_drive_separator) {
    strcat(directory->d_name, dir_separator_and_wildcard);
  } else {
    strcat(directory->d_name, wildcard);
  }

  if (!internal::open_dir_stream(directory)) {
    internal::free_dir(directory);
    return nullptr;
  }

  return directory;
}


// Implementation of the standard POSIX function. See documentation[1].
//
// On success: returns a pointer to the next directory entry, or `nullptr` if
// we've reached the end of the stream.
//
// On failure: returns `nullptr` and sets `errno`.
//
// NOTE: as with most POSIX implementations of this function, you must reset
// `errno` before calling `readdir`.
//
// [1] http://www.gnu.org/software/libc/manual/html_node/Reading_002fClosing-Directory.html#Reading_002fClosing-Directory
inline struct dirent* readdir(DIR* directory)
{
  if (directory == nullptr) {
    errno = EBADF;
    return nullptr;
  }

  if (!internal::reentrant_advance_dir_stream(directory)) {
    return nullptr;
  }

  return &directory->curr;
}


// Implementation of the standard POSIX function. See documentation[1].
//
// On success, return 0; on failure, return -1 and set `errno` appropriately.
//
// [1] http://www.gnu.org/software/libc/manual/html_node/Reading_002fClosing-Directory.html#Reading_002fClosing-Directory
inline int closedir(DIR* directory)
{
  if (directory == nullptr) {
    errno = EBADF;
    return -1;
  }

  BOOL search_closed = false;

  if (directory->handle != INVALID_HANDLE_VALUE) {
    search_closed = FindClose(directory->handle);
  }

  internal::free_dir(directory);

  return search_closed ? 0 : -1;
}

namespace internal {

inline void free_dir(DIR* directory)
{
  if (directory != nullptr) {
    free(directory->d_name);
  }

  free(directory);
}


inline bool open_dir_stream(DIR* directory)
{
  assert(directory != nullptr);

  directory->handle = FindFirstFile(directory->d_name, &directory->fd);

  if (directory->handle == INVALID_HANDLE_VALUE) {
    errno = ENOENT;
    return false;
  }

  // NOTE: `d_name` can be a statically-sized array of `MAX_PATH` size because
  // `cFileName` is. See[1]. This simplifies this copy operation because we
  // don't have to `malloc`.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365740(v=vs.85).aspx
  strcpy(directory->curr.d_name, directory->fd.cFileName);
  directory->curr.d_namlen = strlen(directory->curr.d_name);

  return true;
}


inline bool reentrant_advance_dir_stream(DIR* directory)
{
  assert(directory != nullptr);

  if (!FindNextFile(directory->handle, &directory->fd)) {
    return false;
  }

  // NOTE: `d_name` can be a statically-sized array of `MAX_PATH` size because
  // `cFileName` is. See[1]. This simplifies this copy operation because we
  // don't have to `malloc`.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365740(v=vs.85).aspx
  strcpy(directory->curr.d_name, directory->fd.cFileName);
  directory->curr.d_namlen = strlen(directory->curr.d_name);

  return true;
}

} // namespace internal {
} // extern "C" {


#endif // __STOUT_INTERNAL_WINDOWS_DIRENT_HPP__
