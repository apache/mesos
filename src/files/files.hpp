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

#ifndef __FILES_HPP__
#define __FILES_HPP__

#ifdef __WINDOWS__
#include <stout/internal/windows/grp.hpp>
#include <stout/internal/windows/pwd.hpp>
#else
#include <grp.h>
#include <pwd.h>
#endif // __WINDOWS__

#include <sys/stat.h>

#include <string>

#include <mesos/authorizer/authorizer.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/format.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>

#include <stout/os/permissions.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

namespace mesos {
namespace internal {

// Forward declarations.
class FilesProcess;


// Provides an abstraction for browsing and reading files via HTTP
// endpoints. A path (file or directory) may be "attached" to a name
// (similar to "mounting" a device) for subsequent browsing and
// reading of any files and directories it contains. The "mounting" of
// paths to names enables us to do a form of chrooting for better
// security and isolation of files.
class Files
{
public:
  Files(const Option<std::string>& authenticationRealm = None(),
        const Option<mesos::Authorizer*>& authorizer = None());
  ~Files();

  // Returns the result of trying to attach the specified path
  // (directory or file) at the specified name.
  process::Future<Nothing> attach(
      const std::string& path,
      const std::string& name,
      const Option<lambda::function<
          process::Future<bool>(const Option<std::string>&)>>&
              authorized = None());

  // Removes the specified name.
  void detach(const std::string& name);

private:
  FilesProcess* process;
};


// Returns our JSON representation of a file or directory.
// The JSON contains all of the information one would find in ls -l.
// Example JSON:
// {
//   'path': '\/some\/file',
//   'mode': '-rwxrwxrwx',
//   'nlink': 5,
//   'uid': 'bmahler',
//   'gid': 'employee',
//   'size': 4096,           // Bytes.
//   'mtime': 1348258116,    // Unix timestamp.
// }
inline JSON::Object jsonFileInfo(const std::string& path,
                                 const struct stat& s)
{
  JSON::Object file;
  file.values["path"] = path;
  file.values["nlink"] = s.st_nlink;
  file.values["size"] = s.st_size;
  file.values["mtime"] = s.st_mtime;

  char filetype;
  if (S_ISREG(s.st_mode)) {
    filetype = '-';
  } else if (S_ISDIR(s.st_mode)) {
    filetype = 'd';
  } else if (S_ISCHR(s.st_mode)) {
    filetype = 'c';
  } else if (S_ISBLK(s.st_mode)) {
    filetype = 'b';
  } else if (S_ISFIFO(s.st_mode)) {
    filetype = 'p';
  } else if (S_ISLNK(s.st_mode)) {
    filetype = 'l';
  } else if (S_ISSOCK(s.st_mode)) {
    filetype = 's';
  } else {
    filetype = '-';
  }

  struct os::Permissions permissions(s.st_mode);

  file.values["mode"] = strings::format(
      "%c%c%c%c%c%c%c%c%c%c",
      filetype,
      permissions.owner.r ? 'r' : '-',
      permissions.owner.w ? 'w' : '-',
      permissions.owner.x ? 'x' : '-',
      permissions.group.r ? 'r' : '-',
      permissions.group.w ? 'w' : '-',
      permissions.group.x ? 'x' : '-',
      permissions.others.r ? 'r' : '-',
      permissions.others.w ? 'w' : '-',
      permissions.others.x ? 'x' : '-').get();

  // NOTE: `getpwuid` and `getgrgid` return `nullptr` on Windows.
  passwd* p = getpwuid(s.st_uid);
  if (p != nullptr) {
    file.values["uid"] = p->pw_name;
  } else {
    file.values["uid"] = stringify(s.st_uid);
  }

  struct group* g = getgrgid(s.st_gid);
  if (g != nullptr) {
    file.values["gid"] = g->gr_name;
  } else {
    file.values["gid"] = stringify(s.st_gid);
  }

  return file;
}

} // namespace internal {
} // namespace mesos {

#endif // __FILES_HPP__
