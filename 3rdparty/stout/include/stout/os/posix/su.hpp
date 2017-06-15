// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_POSIX_SU_HPP__
#define __STOUT_OS_POSIX_SU_HPP__

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

namespace os {

inline Result<uid_t> getuid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getuid();
  }

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    struct passwd pwd;
    struct passwd* result;
    char* buffer = new char[size];

    if (getpwnam_r(user->c_str(), &pwd, buffer, size, &result) == 0) {
      // Per POSIX, if the user name is not found, `getpwnam_r` returns
      // zero and sets `result` to the null pointer. (Linux behaves
      // differently for invalid user names; see below).
      if (result == nullptr) {
        delete[] buffer;
        return None();
      }

      // Entry found.
      uid_t uid = pwd.pw_uid;
      delete[] buffer;
      return uid;
    } else {
      delete[] buffer;

      if (errno == ERANGE) {
        // Buffer too small; enlarge it and retry.
        size *= 2;
        continue;
      }

      // According to POSIX, a non-zero return value from `getpwnam_r`
      // indicates an error. However, some versions of glibc return
      // non-zero and set errno to ENOENT, ESRCH, EBADF, EPERM,
      // EINVAL, or other values if the user name was invalid and/or
      // not found. POSIX and Linux manpages also list certain errno
      // values (e.g., EIO, EMFILE) as definitely indicating an error.
      //
      // Hence, we check for those specific error values and return an
      // error to the caller; for any errno value not in that list, we
      // assume the user name wasn't found.
      //
      // TODO(neilc): Consider retrying on EINTR.
      if (errno != EIO &&
          errno != EINTR &&
          errno != EMFILE &&
          errno != ENFILE &&
          errno != ENOMEM) {
        return None();
      }

      return ErrnoError("Failed to get username information");
    }
  }

  UNREACHABLE();
}


inline Try<Nothing> setuid(uid_t uid)
{
  if (::setuid(uid) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Result<gid_t> getgid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getgid();
  }

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    struct passwd pwd;
    struct passwd* result;
    char* buffer = new char[size];

    if (getpwnam_r(user->c_str(), &pwd, buffer, size, &result) == 0) {
      // Per POSIX, if the user name is not found, `getpwnam_r` returns
      // zero and sets `result` to the null pointer. (Linux behaves
      // differently for invalid user names; see below).
      if (result == nullptr) {
        delete[] buffer;
        return None();
      }

      // Entry found.
      gid_t gid = pwd.pw_gid;
      delete[] buffer;
      return gid;
    } else {
      delete[] buffer;

      if (errno == ERANGE) {
        // Buffer too small; enlarge it and retry.
        size *= 2;
        continue;
      }

      // According to POSIX, a non-zero return value from `getpwnam_r`
      // indicates an error. However, some versions of glibc return
      // non-zero and set errno to ENOENT, ESRCH, EBADF, EPERM,
      // EINVAL, or other values if the user name was invalid and/or
      // not found. POSIX and Linux manpages also list certain errno
      // values (e.g., EIO, EMFILE) as definitely indicating an error.
      //
      // Hence, we check for those specific error values and return an
      // error to the caller; for any errno value not in that list, we
      // assume the user name wasn't found.
      //
      // TODO(neilc): Consider retrying on EINTR.
      if (errno != EIO &&
          errno != EINTR &&
          errno != EMFILE &&
          errno != ENFILE &&
          errno != ENOMEM) {
        return None();
      }

      return ErrnoError("Failed to get username information");
    }
  }

  UNREACHABLE();
}


inline Try<Nothing> setgid(gid_t gid)
{
  if (::setgid(gid) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<std::vector<gid_t>> getgrouplist(const std::string& user)
{
  // TODO(jieyu): Consider adding a 'gid' parameter and avoid calling
  // getgid here. In some cases, the primary gid might be known.
  Result<gid_t> gid = os::getgid(user);
  if (!gid.isSome()) {
    return Error("Failed to get the gid of the user: " +
                 (gid.isError() ? gid.error() : "group not found"));
  }

#ifdef __APPLE__
  // TODO(gilbert): Instead of setting 'ngroups' as a large value,
  // we should figure out a way to probe 'ngroups' on OS X. Currently
  // neither '_SC_NGROUPS_MAX' nor 'NGROUPS_MAX' is appropriate,
  // because both are fixed as 16 on Darwin kernel, which is the
  // cache size.
  int ngroups = 65536;
  int gids[ngroups];
#else
  int ngroups = NGROUPS_MAX;
  gid_t gids[ngroups];
#endif
  if (::getgrouplist(user.c_str(), gid.get(), gids, &ngroups) == -1) {
    return ErrnoError();
  }

  return std::vector<gid_t>(gids, gids + ngroups);
}


inline Try<Nothing> setgroups(
    const std::vector<gid_t>& gids,
    const Option<uid_t>& uid = None())
{
  int ngroups = static_cast<int>(gids.size());
  gid_t _gids[ngroups];

  for (int i = 0; i < ngroups; i++) {
    _gids[i] = gids[i];
  }

#ifdef __APPLE__
  // Cannot simply call 'setgroups' here because it only updates
  // the list of groups in kernel cache, but not the ones in
  // opendirectoryd. Darwin kernel caches part of the groups in
  // kernel, and the rest in opendirectoryd.
  // For more detail please see:
  // https://github.com/practicalswift/osx/blob/master/src/samba/patches/support-darwin-initgroups-syscall // NOLINT
  int maxgroups = sysconf(_SC_NGROUPS_MAX);
  if (maxgroups == -1) {
    return Error("Failed to get sysconf(_SC_NGROUPS_MAX)");
  }

  if (ngroups > maxgroups) {
    ngroups = maxgroups;
  }

  if (uid.isNone()) {
    return Error(
        "The uid of the user who is associated with the group "
        "list we are setting is missing");
  }

  // NOTE: By default, the maxgroups on Darwin kernel is fixed
  // as 16. If we have more than 16 gids to set for a specific
  // user, then SYS_initgroups would send up to 16 of them to
  // kernel cache, while the rest would still be performed
  // correctly by the kernel (asking Directory Service to resolve
  // the groups membership).
  if (::syscall(SYS_initgroups, ngroups, _gids, uid.get()) == -1) {
    return ErrnoError();
  }
#else
  if (::setgroups(ngroups, _gids) == -1) {
    return ErrnoError();
  }
#endif

  return Nothing();
}


inline Result<std::string> user(Option<uid_t> uid = None())
{
  if (uid.isNone()) {
    uid = ::getuid();
  }

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    struct passwd pwd;
    struct passwd* result;
    char* buffer = new char[size];

    if (getpwuid_r(uid.get(), &pwd, buffer, size, &result) == 0) {
      // getpwuid_r will return 0 but set result == nullptr if the uid is
      // not found.
      if (result == nullptr) {
        delete[] buffer;
        return None();
      }

      std::string user(pwd.pw_name);
      delete[] buffer;
      return user;
    } else {
      delete[] buffer;

      if (errno != ERANGE) {
        return ErrnoError();
      }

      // getpwuid_r set ERANGE so try again with a larger buffer.
      size *= 2;
    }
  }
}


inline Try<Nothing> su(const std::string& user)
{
  Result<gid_t> gid = os::getgid(user);
  if (gid.isError() || gid.isNone()) {
    return Error("Failed to getgid: " +
        (gid.isError() ? gid.error() : "unknown user"));
  } else if (::setgid(gid.get())) {
    return ErrnoError("Failed to set gid");
  }

  // Set the supplementary group list. We ignore EPERM because
  // performing a no-op call (switching to same group) still
  // requires being privileged, unlike 'setgid' and 'setuid'.
  if (::initgroups(user.c_str(), gid.get()) == -1 && errno != EPERM) {
    return ErrnoError("Failed to set supplementary groups");
  }

  Result<uid_t> uid = os::getuid(user);
  if (uid.isError() || uid.isNone()) {
    return Error("Failed to getuid: " +
        (uid.isError() ? uid.error() : "unknown user"));
  } else if (::setuid(uid.get())) {
    return ErrnoError("Failed to setuid");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_SU_HPP__
