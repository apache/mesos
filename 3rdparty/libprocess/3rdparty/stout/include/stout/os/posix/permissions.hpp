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
#ifndef __STOUT_OS_POSIX_PERMISSIONS_HPP__
#define __STOUT_OS_POSIX_PERMISSIONS_HPP__

#include <sys/stat.h>

#include <string>


namespace os {

struct Permissions
{
  explicit Permissions(mode_t mode)
  {
    owner.r = mode & S_IRUSR;
    owner.w = mode & S_IWUSR;
    owner.x = mode & S_IXUSR;
    owner.rwx = mode & S_IRWXU;
    group.r = mode & S_IRGRP;
    group.w = mode & S_IWGRP;
    group.x = mode & S_IXGRP;
    group.rwx = mode & S_IRWXG;
    others.r = mode & S_IROTH;
    others.w = mode & S_IWOTH;
    others.x = mode & S_IXOTH;
    others.rwx = mode & S_IRWXO;
    setuid = mode & S_ISUID;
    setgid = mode & S_ISGID;
    sticky = mode & S_ISVTX;
  }

  struct {
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
  struct stat s;
  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError();
  }
  return Permissions(s.st_mode);
}

} // namespace os {

#endif // __STOUT_OS_POSIX_PERMISSIONS_HPP__
