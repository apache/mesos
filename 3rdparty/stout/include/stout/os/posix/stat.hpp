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

#ifndef __STOUT_OS_POSIX_STAT_HPP__
#define __STOUT_OS_POSIX_STAT_HPP__

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <string>

#include <stout/bytes.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/int_fd.hpp>


namespace os {

namespace stat {

// Specify whether symlink path arguments should be followed or
// not. APIs in the os::stat family that take a FollowSymlink
// argument all provide FollowSymlink::FOLLOW_SYMLINK as the default value,
// so they will follow symlinks unless otherwise specified.
enum class FollowSymlink
{
  DO_NOT_FOLLOW_SYMLINK,
  FOLLOW_SYMLINK
};


namespace internal {

inline Try<struct ::stat> stat(
    const std::string& path,
    const FollowSymlink follow)
{
  struct ::stat s;

  switch (follow) {
    case FollowSymlink::DO_NOT_FOLLOW_SYMLINK:
      if (::lstat(path.c_str(), &s) < 0) {
        return ErrnoError("Failed to lstat '" + path + "'");
      }
      return s;
    case FollowSymlink::FOLLOW_SYMLINK:
      if (::stat(path.c_str(), &s) < 0) {
        return ErrnoError("Failed to stat '" + path + "'");
      }
      return s;
  }

  UNREACHABLE();
}


inline Try<struct ::stat> stat(const int_fd fd)
{
  struct ::stat s;

  if (::fstat(fd, &s) < 0) {
    return ErrnoError();
  }
  return s;
}

} // namespace internal {

inline bool islink(const std::string& path)
{
  // By definition, you don't follow symlinks when trying
  // to find whether a path is a link. If you followed it,
  // it wouldn't ever be a link.
  Try<struct ::stat> s = internal::stat(
      path, FollowSymlink::DO_NOT_FOLLOW_SYMLINK);
  return s.isSome() && S_ISLNK(s->st_mode);
}


inline bool isdir(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  return s.isSome() && S_ISDIR(s->st_mode);
}


// TODO(andschwa): Share logic with other overload.
inline bool isdir(const int_fd fd)
{
  Try<struct ::stat> s = internal::stat(fd);
  return s.isSome() && S_ISDIR(s->st_mode);
}


inline bool isfile(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  return s.isSome() && S_ISREG(s->st_mode);
}


inline bool issocket(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  return s.isSome() && S_ISSOCK(s->st_mode);
}


// Returns the size in Bytes of a given file system entry. When
// applied to a symbolic link with `follow` set to
// `DO_NOT_FOLLOW_SYMLINK`, this will return the length of the entry
// name (strlen).
inline Try<Bytes> size(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return Bytes(s->st_size);
}


// TODO(andschwa): Share logic with other overload.
inline Try<Bytes> size(const int_fd fd)
{
  Try<struct ::stat> s = internal::stat(fd);
  if (s.isError()) {
    return Error(s.error());
  }

  return Bytes(s->st_size);
}


inline Try<long> mtime(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return s->st_mtime;
}


inline Try<mode_t> mode(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return s->st_mode;
}


inline Try<dev_t> dev(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return s->st_dev;
}


inline Try<dev_t> rdev(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  if (!S_ISCHR(s->st_mode) && !S_ISBLK(s->st_mode)) {
    return Error("Not a special file: " + path);
  }

  return s->st_rdev;
}


inline Try<ino_t> inode(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return s->st_ino;
}


inline Try<uid_t> uid(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  Try<struct ::stat> s = internal::stat(path, follow);
  if (s.isError()) {
    return Error(s.error());
  }

  return s->st_uid;
}

} // namespace stat {

} // namespace os {

#endif // __STOUT_OS_POSIX_STAT_HPP__
