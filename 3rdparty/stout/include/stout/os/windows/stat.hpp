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

#ifndef __STOUT_OS_WINDOWS_STAT_HPP__
#define __STOUT_OS_WINDOWS_STAT_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp>

#include <stout/internal/windows/reparsepoint.hpp>
#include <stout/internal/windows/symlink.hpp>

#ifdef _USE_32BIT_TIME_T
#error "Implementation of `os::stat::mtime` assumes 64-bit `time_t`."
#endif // _USE_32BIT_TIME_T


namespace os {
namespace stat {

inline bool isdir(const std::string& path)
{
  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return false;
  }

  return S_ISDIR(s.st_mode);
}


inline bool isfile(const std::string& path)
{
  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return false;
  }

  return S_ISREG(s.st_mode);
}


inline bool islink(const std::string& path)
{
  Try<::internal::windows::SymbolicLink> symlink =
    ::internal::windows::query_symbolic_link_data(path);

  return symlink.isSome();
}


// Describes the different semantics supported for the implementation
// of `size` defined below.
enum FollowSymlink
{
  DO_NOT_FOLLOW_SYMLINK,
  FOLLOW_SYMLINK
};


// Returns the size in Bytes of a given file system entry. When
// applied to a symbolic link with `follow` set to
// `DO_NOT_FOLLOW_SYMLINK`, this will return the length of the entry
// name (strlen).
inline Try<Bytes> size(
    const std::string& path,
    const FollowSymlink follow = FOLLOW_SYMLINK)
{
  switch (follow) {
    case DO_NOT_FOLLOW_SYMLINK: {
      Try<::internal::windows::SymbolicLink> symlink =
        ::internal::windows::query_symbolic_link_data(path);

      if (symlink.isError()) {
        return Error(symlink.error());
      } else {
        return Bytes(symlink.get().substitute_name.length());
      }
      break;
    }
    case FOLLOW_SYMLINK: {
      struct _stat s;

      if (::_stat(path.c_str(), &s) < 0) {
        return ErrnoError("Error invoking stat for '" + path + "'");
      } else {
        return Bytes(s.st_size);
      }
      break;
    }
  }

  UNREACHABLE();
}


inline Try<long> mtime(const std::string& path)
{
  Try<::internal::windows::SymbolicLink> symlink =
    ::internal::windows::query_symbolic_link_data(path);

  if (symlink.isSome()) {
    return Error(
        "Requested mtime for '" + path +
        "', but symbolic links don't have an mtime on Windows");
  }

  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  // To be safe, we assert that `st_mtime` is represented as `__int64`. To
  // conform to the POSIX, we also cast `st_mtime` to `long`; we choose to make
  // this conversion explicit because we expect the truncation to not cause
  // information loss.
  static_assert(
      std::is_same<__int64, __time64_t>::value,
      "Mesos assumes `__time64_t` is represented as `__int64`");
  return static_cast<long>(s.st_mtime);
}


inline Try<mode_t> mode(const std::string& path)
{
  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_mode;
}


inline Try<dev_t> dev(const std::string& path)
{
  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_dev;
}


inline Try<ino_t> inode(const std::string& path)
{
  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_ino;
}

} // namespace stat {
} // namespace os {

#endif // __STOUT_OS_WINDOWS_STAT_HPP__
