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

#ifndef __STOUT_POSIX_FS_HPP__
#define __STOUT_POSIX_FS_HPP__

#include <unistd.h> // For symlink.
#include <sys/statvfs.h>

#include <string>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

// TODO(bmahler): Merge available() and usage() into df() that returns
// a struct, and move this back into os.hpp.
namespace fs {

// Returns the total disk size in bytes.
inline Try<Bytes> size(const std::string& path = "/")
{
  struct statvfs buf;
  if (::statvfs(path.c_str(), &buf) < 0) {
    return ErrnoError();
  }
  return Bytes(buf.f_frsize) * buf.f_blocks;
}


// Returns the amount of disk space used in bytes.
inline Try<Bytes> used(const std::string& path = "/")
{
  struct statvfs buf;
  if (::statvfs(path.c_str(), &buf) < 0) {
    return ErrnoError();
  }
  return Bytes(buf.f_frsize) * (buf.f_blocks - buf.f_bfree);
}


// Returns relative disk usage of the file system that the given path
// is mounted at.
inline Try<double> usage(const std::string& path = "/")
{
  struct statvfs buf;
  if (statvfs(path.c_str(), &buf) < 0) {
    return ErrnoError("Error invoking statvfs on '" + path + "'");
  }
  return (double) (buf.f_blocks - buf.f_bfree) / buf.f_blocks;
}


inline Try<Nothing> symlink(
    const std::string& original,
    const std::string& link)
{
  if (::symlink(original.c_str(), link.c_str()) < 0) {
    return ErrnoError();
  }
  return Nothing();
}


// Returns a list of all files matching the given pattern. On POSIX builds this
// is just a wrapper on os::glob().
//
// This function was added is because glob() is not available on Windows, and we
// are not making use of any of the heavyweight features of ::glob() anyway.
//
// list() is meant to be a lightweight alternative to glob() - the only
// wildcards it should be used with are `?` and `*`, and only when they appear
// at the tail end of `pattern` (e.g. `/root/dir/subdir/*.txt` or
// `/root/dir/subdir/file?.txt`.
inline Try<std::list<std::string>> list(const std::string& pattern)
{
  return os::glob(pattern);
}

} // namespace fs {

#endif // __STOUT_POSIX_FS_HPP__
