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
#ifndef __STOUT_OS_STAT_HPP__
#define __STOUT_OS_STAT_HPP__

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <string>

#include <stout/try.hpp>

namespace os {
namespace stat {

inline bool isdir(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISDIR(s.st_mode);
}


inline bool isfile(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISREG(s.st_mode);
}



inline bool islink(const std::string& path)
{
  struct stat s;

  if (::lstat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISLNK(s.st_mode);
}


inline Try<long> mtime(const std::string& path)
{
  struct stat s;

  if (::lstat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_mtime;
}


inline Try<mode_t> mode(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_mode;
}


inline Try<dev_t> rdev(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  if (!S_ISCHR(s.st_mode) && !S_ISBLK(s.st_mode)) {
    return Error("Not a special file: " + path);
  }

  return s.st_rdev;
}


inline Try<ino_t> inode(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_ino;
}

} // namespace stat {
} // namespace os {
#endif // __STOUT_OS_STAT_HPP__
