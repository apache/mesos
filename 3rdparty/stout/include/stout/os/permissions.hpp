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

#ifndef __STOUT_OS_PERMISSIONS_HPP__
#define __STOUT_OS_PERMISSIONS_HPP__

#include <sys/stat.h>

#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>


namespace os {

struct Permissions
{
  explicit Permissions(mode_t mode)
  {
    owner.r = (mode & S_IRUSR) != 0;
    owner.w = (mode & S_IWUSR) != 0;
    owner.x = (mode & S_IXUSR) != 0;
    owner.rwx = (mode & S_IRWXU) != 0;
    group.r = (mode & S_IRGRP) != 0;
    group.w = (mode & S_IWGRP) != 0;
    group.x = (mode & S_IXGRP) != 0;
    group.rwx = (mode & S_IRWXG) != 0;
    others.r = (mode & S_IROTH) != 0;
    others.w = (mode & S_IWOTH) != 0;
    others.x = (mode & S_IXOTH) != 0;
    others.rwx = (mode & S_IRWXO) != 0;
    setuid = (mode & S_ISUID) != 0;
    setgid = (mode & S_ISGID) != 0;
    sticky = (mode & S_ISVTX) != 0;
  }

  struct
  {
    bool r;
    bool w;
    bool x;
    bool rwx;
  } owner, group, others;

  bool setuid;
  bool setgid;
  bool sticky;
};


inline Try<Permissions> permissions(const std::string& path)
{
#ifdef __WINDOWS__
  VLOG(2) << "`os::permissions` has been called, but is a stub on Windows";
  return Permissions(0);
#else
  struct stat status;
  if (::stat(path.c_str(), &status) < 0) {
    return ErrnoError();
  }

  return Permissions(status.st_mode);
#endif // __WINDOWS__
}


} // namespace os {


#endif // __STOUT_OS_PERMISSIONS_HPP__
