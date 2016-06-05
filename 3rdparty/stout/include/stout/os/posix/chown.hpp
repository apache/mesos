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

#ifndef __STOUT_OS_POSIX_CHOWN_HPP__
#define __STOUT_OS_POSIX_CHOWN_HPP__

#include <sys/types.h>
#include <pwd.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/shell.hpp>

namespace os {

inline Try<Nothing> chown(
    uid_t uid,
    gid_t gid,
    const std::string& path,
    bool recursive)
{
  if (recursive) {
    // TODO(bmahler): Consider walking the file tree instead. We would need
    // to be careful to not miss dotfiles.
    std::string command =
      "chown -R " + stringify(uid) + ':' + stringify(gid) + " '" + path + "'";

    int status = os::system(command);
    if (status != 0) {
      return ErrnoError(
          "Failed to execute '" + command +
          "' (exit status: " + stringify(status) + ")");
    }
  } else {
    if (::chown(path.c_str(), uid, gid) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
}


// Changes the specified path's user and group ownership to that of
// the specified user.
inline Try<Nothing> chown(
    const std::string& user,
    const std::string& path,
    bool recursive = true)
{
  passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == nullptr) {
    return ErrnoError("Failed to get user information for '" + user + "'");
  }

  return chown(passwd->pw_uid, passwd->pw_gid, path, recursive);
}

} // namespace os {

#endif // __STOUT_OS_POSIX_CHOWN_HPP__
